#pragma once

// ninfer::ops - NVFP4 (4-bit E2M1 + E4M3 scale) group-16 KV cache codec (shared
// device helpers). Quantization (append) and dequantization (stage) are FUSED into
// the GQA attention kernels themselves (decode partial kernel, prefill fill/attention);
// this header only provides the index math, the vectorized dequant, and the scalar
// quantize helper they share. There is deliberately no standalone quant/dequant
// kernel: that would defeat the halved-bandwidth goal.
//
// Storage layout (opaque DType::U8 planes):
//   Code  plane: leading_extent = head_dim / 2  (2 E2M1 elements per byte)
//   Scale plane: leading_extent = head_dim / 16  (1 E4M3 byte per group of 16)
// With head_dim = 256: code leading = 128 bytes, scale leading = 16 bytes.
// Per token the KV payload is 128 + 16 = 144 bytes vs. 256 bytes for int8
// (256 + 8) and 512 bytes for bf16.

#include "ops/common/math.cuh"
#include "ops/common/memory.cuh"
#include "ops/kernel/paged_kv_address.cuh"
#include "ops/linear/nvfp4/nvfp4_codec.cuh"

#include <cuda_bf16.h>
#include <cuda_fp16.h>

#include <cstdint>

namespace ninfer::ops {

inline constexpr int kGqaKvNvfp4HeadDim = 256;
inline constexpr int kGqaKvNvfp4Group   = 16;
inline constexpr int kGqaKvNvfp4Groups  = kGqaKvNvfp4HeadDim / kGqaKvNvfp4Group;

// Code index: leading extent = head_dim / 2 (byte-addressable packed E2M1, 2
// elements per byte).
template <typename Geometry>
__device__ __forceinline__ std::int64_t
gqa_kv_nvfp4_code_index(int physical_page, int kv_head, int d, int page_offset) {
    return paged_kv_element_offset<kGqaKvNvfp4HeadDim / 2, Geometry::KVHeads>(
        physical_page, kv_head, page_offset, d);
}

// Scale index: leading extent = head_dim / 16 (one E4M3 scale byte per group).
template <typename Geometry>
__device__ __forceinline__ std::int64_t
gqa_kv_nvfp4_scale_index(int physical_page, int kv_head, int group, int page_offset) {
    return paged_kv_element_offset<kGqaKvNvfp4Groups, Geometry::KVHeads>(
        physical_page, kv_head, page_offset, group);
}

// Source index in the contiguous bf16 K/V output tensor (head_dim-major per head).
template <typename Geometry>
__device__ __forceinline__ std::int64_t gqa_kv_nvfp4_src_index(int kv_head, int d, int token) {
    return static_cast<std::int64_t>(d) +
           static_cast<std::int64_t>(kGqaKvNvfp4HeadDim) *
               (static_cast<std::int64_t>(kv_head) +
                static_cast<std::int64_t>(Geometry::KVHeads) * token);
}

// Dequantize 8 consecutive NVFP4 E2M1 codes (4 bytes, dims [d, d+8), aligned to a
// multiple of 8 so they lie inside one 16-element group) into 8 bf16 packed as an
// int4, given a pointer to the 4 code bytes and the group's E4M3 dequant scale.
// The 4 code bytes are read with ONE 32-bit load; the pointer may be in global or
// shared memory. This keeps the dequant ALU identical whether the codes were
// streamed via cp.async into smem (decode) or read directly from the cache
// (prefill).
//
// Each byte holds two E2M1 values; decode_nvfp4_e2m1x2 returns a float2. The E4M3
// scale byte is decoded to a float and multiplied to recover the original value.
__device__ __forceinline__ int4 gqa_kv_nvfp4_dequant_e2m1x8_from(const std::uint8_t* codes4,
                                                                  std::uint8_t scale_byte) {
    const float s = detail::decode_nvfp4_e4m3(scale_byte);
    const std::uint32_t raw = load_vec<std::uint32_t>(codes4);
    const std::uint8_t* c   = reinterpret_cast<const std::uint8_t*>(&raw);
    unsigned packed[4];
#pragma unroll
    for (int i = 0; i < 4; ++i) {
        const float2 v0 = detail::decode_nvfp4_e2m1x2(c[i]);
        const float x0  = v0.x * s;
        const float x1  = v0.y * s;
        packed[i]       = pack_bf16x2(x0, x1);
    }
    return make_int4(static_cast<int>(packed[0]), static_cast<int>(packed[1]),
                     static_cast<int>(packed[2]), static_cast<int>(packed[3]));
}

// Quantize 16 bf16 values into 8 bytes of packed E2M1 codes + 1 E4M3 scale byte.
// This is a thin wrapper around quantize_nvfp4_k16 with input_scale_divisor = 1.0
// (no external scaling; the codec computes the per-group absmax and the E4M3 scale
// internally). The 16-element group is the NVFP4 super-block.
//
// source: pointer to 16 consecutive __nv_bfloat16 values (32 bytes, head_dim/16
//         of one group within one head).
// code_dst: destination for 8 packed E2M1 bytes (must be 8-byte aligned for the
//           uint2 store).
// scale_dst: destination for 1 E4M3 scale byte.
__device__ __forceinline__ void gqa_kv_nvfp4_quantize_k16(const __nv_bfloat16* source,
                                                           std::uint8_t* code_dst,
                                                           std::uint8_t* scale_dst) {
    const detail::Nvfp4QuantizedK16 result =
        detail::quantize_nvfp4_k16(source, 1.0F);
    store_vec(code_dst, make_uint2(result.codes_lo, result.codes_hi));
    *scale_dst = result.scale;
}

} // namespace ninfer::ops
