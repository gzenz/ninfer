#pragma once

#include "core/tensor.h"

#include <cuda_runtime.h>

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
    cudaStream_t stream);

} // namespace ninfer::ops::detail
