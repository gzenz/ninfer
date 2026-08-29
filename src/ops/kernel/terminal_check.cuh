#pragma once

// Implements: include/ninfer/ops/terminal_check.h
// Match: per-row stop-token and budget terminal detection for MTP and ordinary decode.

#include "ninfer/types.h"

#include <cstdint>

namespace ninfer::ops {

__global__ void terminal_check_kernel(
    const std::int32_t* licensed_tokens,
    const std::int32_t* licensed_counts,
    const std::int32_t* remaining_budgets,
    const std::int32_t* stop_token_table,
    const std::int32_t* stop_token_counts,
    std::int32_t* committed_counts,
    std::int32_t* terminal_flags,
    std::int32_t batch,
    std::int32_t width) {
    const int row = blockIdx.y;
    if (row >= batch) return;

    if (blockIdx.x == 0 && threadIdx.x == 0) {
        const int count     = (width == 1) ? 1 : licensed_counts[row];
        const int budget    = remaining_budgets[row];
        const int num_stops = stop_token_counts[row];

        // Find first stop token in licensed_tokens[row, 0..count-1]
        int stop_pos = count; // default: no stop found
        for (int j = 0; j < count; ++j) {
            const std::int32_t token = licensed_tokens[row * width + j];
            for (int s = 0; s < num_stops; ++s) {
                if (token == stop_token_table[row * static_cast<int>(kMaxStopTokens) + s]) {
                    stop_pos = j;
                    break;
                }
            }
            if (stop_pos < count) break;
        }

        const bool stop_found   = stop_pos < count;
        const int  budget_limit = budget < count ? budget : count;
        int committed;
        if (stop_found) {
            committed = stop_pos + 1;
            if (committed > budget_limit) committed = budget_limit;
        } else {
            committed = budget_limit;
        }

        const bool terminal   = stop_found || (budget <= count);
        committed_counts[row] = committed;
        terminal_flags[row]  = terminal ? 1 : 0;
    }
}

} // namespace ninfer::ops
