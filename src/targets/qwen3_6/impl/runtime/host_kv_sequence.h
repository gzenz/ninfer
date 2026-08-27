#pragma once

#include "core/host_kv_budget.h"
#include "core/linear_attention_state.h"
#include "core/paged_kv_cache.h"
#include "core/tensor.h"

#include "core/device.h"

#include <cuda_runtime_api.h>

#include <cstdint>
#include <vector>

namespace ninfer::targets::qwen3_6::detail {

// Everything about a parked sequence that does not live in its KV pages.
//
// clear_lane() drops the KV *and* the ledger, frontiers, prefix identity and
// rewrite checkpoint. The planner reads exactly those to choose append_frontier
// over a full reset, so a park that saved only pages would restore a sequence
// the planner then treats as empty. This captures the rest.
//
// The per-lane state that is NOT in the KV pages must all ride in the slab, or
// a restore puts a sequence back with the wrong continuation state:
//
//  - the two hidden-state tensors (per-lane slices of a persistent store);
//  - the GDN (linear-attention) conv + recurrent state, for both the current
//    slot and the rewrite-checkpoint slot. That state is lane-affine: a side
//    request that runs in the same lane zeroes the current slot via
//    ordered_reset(), so without parking it the restored sequence would
//    continue from a zeroed recurrent state and produce wrong tokens.
struct ParkedSequenceMeta {
    std::vector<std::int32_t> text_page_ids;
    std::vector<std::int32_t> backend_page_ids;
    std::uint32_t text_page_entitlement    = 0;
    std::uint32_t backend_page_entitlement = 0;
    bool has_backend                       = false;

    std::size_t text_bytes    = 0;  // slab offset where backend pages begin
    std::size_t backend_bytes = 0;
    std::size_t hidden_bytes  = 0;  // tail + rewrite-checkpoint hidden, after the pages
    std::size_t gdn_bytes     = 0;  // GDN conv+recurrent (current + checkpoint slots), after hidden
};

// Bytes a slab must hold for one sequence at the given page entitlements, plus
// the non-paged tail (hidden tensors and GDN state).
[[nodiscard]] inline std::size_t parked_sequence_bytes(const PagedKVPool& text_pool,
                                                       const PagedKVPool* backend_pool,
                                                       std::uint32_t text_pages,
                                                       std::uint32_t backend_pages,
                                                       std::size_t tail_bytes) {
    std::size_t bytes =
        host_kv_bytes_per_page(text_pool.host_kv_plane_views()) * text_pages;
    if (backend_pool != nullptr) {
        bytes += host_kv_bytes_per_page(backend_pool->host_kv_plane_views()) * backend_pages;
    }
    return bytes + tail_bytes;
}

// A chunked sequence image: the bytes a single HostKvSlab used to hold now span
// several slabs, chunk k covering global range [k*chunk, min((k+1)*chunk, total)).
// Sequential writers resolve their cursor through it; a write crossing a chunk
// boundary is split at the boundary (each part lands in exactly one slab).
struct ChunkedImage {
    std::span<HostKvSlab* const> slabs;
    std::size_t chunk = 0;

    [[nodiscard]] unsigned char* at(std::size_t offset) const {
        return static_cast<unsigned char*>(slabs[offset / chunk]->base()) +
               offset % chunk;
    }
    // Bytes from `offset` to the end of its chunk.
    [[nodiscard]] std::size_t room(std::size_t offset) const {
        return chunk - offset % chunk;
    }
    [[nodiscard]] std::size_t total() const { return slabs.size() * chunk; }
};

// Copies a device tensor into the image at `offset`, splitting at chunk edges.
inline void chunked_copy(void* dst_device, const ChunkedImage& image, std::size_t offset,
                         std::size_t bytes, cudaMemcpyKind kind, cudaStream_t stream) {
    std::size_t done = 0;
    while (done < bytes) {
        const std::size_t off = offset + done;
        const std::size_t n = std::min(bytes - done, image.room(off));
        auto* host = image.at(off);
        const auto* dev = static_cast<const unsigned char*>(dst_device) + done;
        CUDA_CHECK(cudaMemcpyAsync(kind == cudaMemcpyDeviceToHost ? host : const_cast<unsigned char*>(dev),
                                   kind == cudaMemcpyDeviceToHost ? dev : host, n, kind, stream));
        done += n;
    }
}

// Chunked variants of the sequential writers/readers below: they address the
// sequence image through ChunkedImage instead of a single slab.
inline std::size_t park_hidden_chunked(const Tensor& tail, const Tensor& checkpoint,
                                       const ChunkedImage& image, std::size_t offset,
                                       cudaStream_t stream) {
    chunked_copy(tail.data, image, offset, tail.bytes(), cudaMemcpyDeviceToHost, stream);
    chunked_copy(checkpoint.data, image, offset + tail.bytes(), checkpoint.bytes(),
                 cudaMemcpyDeviceToHost, stream);
    return tail.bytes() + checkpoint.bytes();
}

inline void restore_hidden_chunked(const Tensor& tail, const Tensor& checkpoint,
                                   const ChunkedImage& image, std::size_t offset,
                                   cudaStream_t stream) {
    chunked_copy(tail.data, image, offset, tail.bytes(), cudaMemcpyHostToDevice, stream);
    chunked_copy(checkpoint.data, image, offset + tail.bytes(), checkpoint.bytes(),
                 cudaMemcpyHostToDevice, stream);
}

inline std::size_t park_gdn_slot_chunked(const LinearAttentionStatePool& pool, std::int32_t slot,
                                         const ChunkedImage& image, std::size_t offset,
                                         cudaStream_t stream) {
    std::size_t written = 0;
    for (std::uint32_t layer = 0; layer < pool.layer_count(); ++layer) {
        const Tensor conv = pool.conv_slot(layer, slot);
        chunked_copy(conv.data, image, offset + written, conv.bytes(), cudaMemcpyDeviceToHost,
                     stream);
        written += conv.bytes();
    }
    for (std::uint32_t layer = 0; layer < pool.layer_count(); ++layer) {
        const Tensor rec = pool.recurrent_slot(layer, slot);
        chunked_copy(rec.data, image, offset + written, rec.bytes(), cudaMemcpyDeviceToHost,
                     stream);
        written += rec.bytes();
    }
    return written;
}

inline void restore_gdn_slot_chunked(const LinearAttentionStatePool& pool, std::int32_t slot,
                                     const ChunkedImage& image, std::size_t offset,
                                     cudaStream_t stream) {
    std::size_t read = 0;
    for (std::uint32_t layer = 0; layer < pool.layer_count(); ++layer) {
        const Tensor conv = pool.conv_slot(layer, slot);
        chunked_copy(conv.data, image, offset + read, conv.bytes(), cudaMemcpyHostToDevice,
                     stream);
        read += conv.bytes();
    }
    for (std::uint32_t layer = 0; layer < pool.layer_count(); ++layer) {
        const Tensor rec = pool.recurrent_slot(layer, slot);
        chunked_copy(rec.data, image, offset + read, rec.bytes(), cudaMemcpyHostToDevice,
                     stream);
        read += rec.bytes();
    }
}

// Copies both hidden-state tensors into the slab after the pages.
inline std::size_t park_hidden(const Tensor& tail, const Tensor& checkpoint, HostKvSlab& slab,
                               std::size_t offset, cudaStream_t stream) {
    auto* cursor = static_cast<unsigned char*>(slab.base()) + offset;
    CUDA_CHECK(cudaMemcpyAsync(cursor, tail.data, tail.bytes(), cudaMemcpyDeviceToHost, stream));
    cursor += tail.bytes();
    CUDA_CHECK(cudaMemcpyAsync(cursor, checkpoint.data, checkpoint.bytes(),
                               cudaMemcpyDeviceToHost, stream));
    return tail.bytes() + checkpoint.bytes();
}

inline void restore_hidden(const Tensor& tail, const Tensor& checkpoint, const HostKvSlab& slab,
                           std::size_t offset, cudaStream_t stream) {
    const auto* cursor = static_cast<const unsigned char*>(slab.base()) + offset;
    CUDA_CHECK(cudaMemcpyAsync(tail.data, cursor, tail.bytes(), cudaMemcpyHostToDevice, stream));
    cursor += tail.bytes();
    CUDA_CHECK(cudaMemcpyAsync(checkpoint.data, cursor, checkpoint.bytes(),
                               cudaMemcpyHostToDevice, stream));
}

// Copies one GDN slot (every layer's conv + recurrent state) into the slab.
// Returns the bytes written. A model with no GDN layers writes nothing.
inline std::size_t park_gdn_slot(const LinearAttentionStatePool& pool, std::int32_t slot,
                                 HostKvSlab& slab, std::size_t offset, cudaStream_t stream) {
    auto* cursor = static_cast<unsigned char*>(slab.base()) + offset;
    for (std::uint32_t layer = 0; layer < pool.layer_count(); ++layer) {
        const Tensor conv = pool.conv_slot(layer, slot);
        CUDA_CHECK(cudaMemcpyAsync(cursor, conv.data, conv.bytes(), cudaMemcpyDeviceToHost, stream));
        cursor += conv.bytes();
    }
    for (std::uint32_t layer = 0; layer < pool.layer_count(); ++layer) {
        const Tensor rec = pool.recurrent_slot(layer, slot);
        CUDA_CHECK(cudaMemcpyAsync(cursor, rec.data, rec.bytes(), cudaMemcpyDeviceToHost, stream));
        cursor += rec.bytes();
    }
    return static_cast<std::size_t>(cursor - (static_cast<unsigned char*>(slab.base()) + offset));
}

inline void restore_gdn_slot(const LinearAttentionStatePool& pool, std::int32_t slot,
                             const HostKvSlab& slab, std::size_t offset, cudaStream_t stream) {
    const auto* cursor = static_cast<const unsigned char*>(slab.base()) + offset;
    for (std::uint32_t layer = 0; layer < pool.layer_count(); ++layer) {
        const Tensor conv = pool.conv_slot(layer, slot);
        CUDA_CHECK(cudaMemcpyAsync(conv.data, cursor, conv.bytes(), cudaMemcpyHostToDevice, stream));
        cursor += conv.bytes();
    }
    for (std::uint32_t layer = 0; layer < pool.layer_count(); ++layer) {
        const Tensor rec = pool.recurrent_slot(layer, slot);
        CUDA_CHECK(cudaMemcpyAsync(rec.data, cursor, rec.bytes(), cudaMemcpyHostToDevice, stream));
        cursor += rec.bytes();
    }
}

}  // namespace ninfer::targets::qwen3_6::detail