#pragma once

// E2M1 (4-bit) + E4M3FN group-16 KV-cache codec shared by standalone append and causal
// Attention.  Two E2M1 elements pack into one byte; one E4M3 scale byte serves each
// group of 16 elements.  Per token per KV head: 128 code bytes + 16 scale bytes = 144
// bytes (vs 264 for int8, 512 for bf16).

#include "ops/common/math.cuh"
#include "ops/common/memory.cuh"
#include "ops/kernel/paged_kv_address.cuh"
#include "ops/kv_cache/hadamard_d256.cuh"
#include "ops/linear/nvfp4/nvfp4_codec.cuh"

#include <cuda_bf16.h>
#include <cuda_fp16.h>

#include <cstdint>

namespace ninfer::ops {

inline constexpr int kKVCacheNvfp4HeadDim  = 256;
inline constexpr int kKVCacheNvfp4Group   = 16;
inline constexpr int kKVCacheNvfp4Groups   = kKVCacheNvfp4HeadDim / kKVCacheNvfp4Group;

// Code index: leading extent = head_dim / 2 (byte-addressable packed E2M1, 2 per byte).
template <typename Geometry>
__device__ __forceinline__ std::int64_t
kv_cache_nvfp4_code_index(int physical_page, int kv_head, int d, int page_offset) {
    return paged_kv_element_offset<kKVCacheNvfp4HeadDim / 2, Geometry::KVHeads>(
        physical_page, kv_head, page_offset, d);
}

// Scale index: leading extent = head_dim / 16 (one E4M3 scale byte per group).
template <typename Geometry>
__device__ __forceinline__ std::int64_t
kv_cache_nvfp4_scale_index(int physical_page, int kv_head, int group, int page_offset) {
    return paged_kv_element_offset<kKVCacheNvfp4Groups, Geometry::KVHeads>(
        physical_page, kv_head, page_offset, group);
}

// Source index in the contiguous bf16 K/V output tensor (head_dim-major per head).
template <typename Geometry>
__device__ __forceinline__ std::int64_t kv_cache_nvfp4_src_index(int kv_head, int d, int token) {
    return static_cast<std::int64_t>(d) +
           static_cast<std::int64_t>(kKVCacheNvfp4HeadDim) *
               (static_cast<std::int64_t>(kv_head) +
                static_cast<std::int64_t>(Geometry::KVHeads) * token);
}

// Dequantize 8 consecutive NVFP4 E2M1 codes (4 bytes, dims [d, d+8), aligned to a
// multiple of 8 so they lie inside one 16-element group) into 8 bf16 packed as an
// int4, given a pointer to the 4 code bytes and the group's E4M3 dequant scale byte.
// The 4 code bytes are read with ONE 32-bit load; the pointer may be in global or
// shared memory.
__device__ __forceinline__ int4 kv_cache_nvfp4_dequant_e2m1x8_from(const std::uint8_t* codes4,
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

// Dequantize 8 consecutive NVFP4 E2M1 codes into 8 fp16 packed as int4.
// Same as above but produces fp16 instead of bf16 for kernels that use fp16 MMA.
__device__ __forceinline__ int4
kv_cache_nvfp4_dequant_e2m1x8_f16_from(const std::uint8_t* codes4, std::uint8_t scale_byte) {
    const float s = detail::decode_nvfp4_e4m3(scale_byte);
    const std::uint32_t raw = load_vec<std::uint32_t>(codes4);
    const std::uint8_t* c   = reinterpret_cast<const std::uint8_t*>(&raw);
    unsigned packed[4];
#pragma unroll
    for (int i = 0; i < 4; ++i) {
        const float2 v0 = detail::decode_nvfp4_e2m1x2(c[i]);
        const __half2 h2 = __float22half2_rn(make_float2(v0.x * s, v0.y * s));
        packed[i] = *reinterpret_cast<const unsigned*>(&h2);
    }
    return make_int4(static_cast<int>(packed[0]), static_cast<int>(packed[1]),
                     static_cast<int>(packed[2]), static_cast<int>(packed[3]));
}

// Quantize 16 bf16 values into 8 bytes of packed E2M1 codes + 1 E4M3 scale byte.
// Thin wrapper around quantize_nvfp4_k16 with input_scale_divisor = 1.0.
__device__ __forceinline__ void kv_cache_nvfp4_quantize_k16(const __nv_bfloat16* source,
                                                            std::uint8_t* code_dst,
                                                            std::uint8_t* scale_dst) {
    const detail::Nvfp4QuantizedK16 result = detail::quantize_nvfp4_k16(source, 1.0F);
    store_vec(code_dst, make_uint2(result.codes_lo, result.codes_hi));
    *scale_dst = result.scale;
}

} // namespace ninfer::ops
