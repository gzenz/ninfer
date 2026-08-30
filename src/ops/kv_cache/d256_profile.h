#pragma once

#include "core/dtype.h"

#include <cstdint>
#include <stdexcept>

namespace ninfer::ops {

inline constexpr std::int32_t kD256KVCacheHeadDim = 256;

struct D256KVCacheProfile {
    DType code_dtype;
    std::int32_t quant_group;
    std::int32_t scale_leading_extent;
    DType scale_dtype;
    std::int32_t code_leading_extent;
};

inline D256KVCacheProfile d256_kv_cache_profile(DType dtype) {
    switch (dtype) {
    case DType::BF16:
        return {DType::BF16, 0, 0, DType::BF16, kD256KVCacheHeadDim};
    case DType::I8:
        return {DType::I8, 64, 4, DType::FP16, kD256KVCacheHeadDim};
    case DType::FP8_E4M3FN:
        return {DType::FP8_E4M3FN, 256, 1, DType::FP16, kD256KVCacheHeadDim};
    case DType::U8:
        return {DType::U8, 16, 16, DType::U8, kD256KVCacheHeadDim / 2};
    default:
        throw std::invalid_argument("unsupported D256 KV-cache dtype");
    }
}

} // namespace ninfer::ops
