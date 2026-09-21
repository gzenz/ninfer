#pragma once

// Dependency-free value-aware victim ranking for the qwen3_5 pressure planner.
//
// Lives in its own light header (no program_impl.h, no model/GPU types) so the exact
// ranking the planner charges to the evict cost can be unit-tested in isolation — the
// entangled caller (PressurePlanningSessionImpl::populate_options) only supplies each
// victim's shared flag and re-prefill cost.

#include <algorithm>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

namespace ninfer::models::qwen3_5::detail {

// Rank private pressure victims by re-prefill cost so the planner's cost objective prefers
// demoting the highest-value (most expensive to rebuild) victims to host over
// evict-and-dropping the cheapest.
//
// Returns one value_weight per victim:
//   - a private victim gets its 0-based rank among the non-shared victims ordered by
//     ascending re-prefill cost (cheapest = 0, most expensive = N-1);
//   - a shared victim is always 0 (shared state is not charged a private re-prefill cost).
//
// `is_shared[i]` is non-zero when victim i is a shared prefix owner; `rebuild_cost[i]` is
// victim i's re-prefill cost (tokens + attention_pairs, already saturated); both are
// ignored for shared victims. Ties keep victim order (the index breaks them, so the result
// is deterministic). `is_shared` and `rebuild_cost` must have equal size.
[[nodiscard]] inline std::vector<std::uint32_t>
value_weights_for_victims(std::span<const std::uint8_t> is_shared,
                          std::span<const std::uint64_t> rebuild_cost) noexcept {
    const std::size_t n = is_shared.size();
    std::vector<std::uint32_t> weights(n, 0U);
    std::vector<std::tuple<std::uint64_t, std::uint32_t>> costs; // (cost, victim_index)
    costs.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (is_shared[i]) { continue; }
        costs.emplace_back(rebuild_cost[i], static_cast<std::uint32_t>(i));
    }
    std::sort(costs.begin(), costs.end());
    for (std::size_t rank = 0; rank < costs.size(); ++rank) {
        weights[std::get<1>(costs[rank])] = static_cast<std::uint32_t>(rank);
    }
    return weights;
}

} // namespace ninfer::models::qwen3_5::detail
