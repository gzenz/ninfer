#pragma once

#include "core/tensor.h"

#include <cuda_runtime.h>

namespace ninfer::ops {

/**
 * Op: terminal_check
 *
 * Math / indexing:
 *   For each row b with L=licensed_counts[b], B=remaining_budgets[b]:
 *     stop_pos = L  (default: no stop found)
 *     for j in 0..L-1:
 *       token = licensed_tokens[b*width + j]
 *       for s in 0..stop_token_counts[b]-1:
 *         if token == stop_token_table[b*kMaxStopTokens + s]:
 *           stop_pos = j; break
 *       if stop_pos < L: break
 *     budget_limit = min(B, L)
 *     committed_counts[b] = stop_pos < L ? min(stop_pos + 1, budget_limit) : budget_limit
 *     terminal_flags[b]   = (stop_pos < L || B <= L) ? 1 : 0
 *
 * Logical shapes / effects:
 *   licensed_tokens is contiguous I32 [width,B]. stop_token_table is contiguous I32
 *   [kMaxStopTokens,B]. All other tensors are contiguous I32 [B]. B>=1, width>=1,
 *   0<=licensed_counts[b]<=width, remaining_budgets[b]>=0,
 *   0<=stop_token_counts[b]<=kMaxStopTokens. The Op writes every output slot.
 *   Inputs remain unchanged. No workspace or other state is used.
 */
void terminal_check(
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

} // namespace ninfer::ops
