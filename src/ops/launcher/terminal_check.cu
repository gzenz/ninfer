// Implements: include/ninfer/ops/terminal_check.h
// Match: per-row stop-token and budget terminal detection.
#include "ops/launcher/terminal_check.h"

#include "core/device.h"
#include "ops/kernel/terminal_check.cuh"

#include <cstdint>

namespace ninfer::ops::detail {

void terminal_check_launch(
    const Tensor& licensed_tokens,
    const Tensor& licensed_counts,
    const Tensor& remaining_budgets,
    const Tensor& stop_token_table,
    const Tensor& stop_token_counts,
    Tensor& committed_counts,
    Tensor& terminal_flags,
    std::int32_t batch,
    std::int32_t width,
    cudaStream_t stream) {
    constexpr int kBlock = 32;
    const dim3 grid(1, static_cast<unsigned int>(batch));
    terminal_check_kernel<<<grid, kBlock, 0, stream>>>(
        static_cast<const std::int32_t*>(licensed_tokens.data),
        static_cast<const std::int32_t*>(licensed_counts.data),
        static_cast<const std::int32_t*>(remaining_budgets.data),
        static_cast<const std::int32_t*>(stop_token_table.data),
        static_cast<const std::int32_t*>(stop_token_counts.data),
        static_cast<std::int32_t*>(committed_counts.data),
        static_cast<std::int32_t*>(terminal_flags.data),
        batch, width);
    CUDA_CHECK(cudaGetLastError());
}

} // namespace ninfer::ops::detail
