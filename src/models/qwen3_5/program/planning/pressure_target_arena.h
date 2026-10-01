#pragma once

// THE PRESSURE TARGET ARENA'S BOUND, as a decision a host-only test can make.
//
// It lives in a header for the same reason `attribute_divergence` does: the rule is what was wrong,
// and the rule is otherwise only reachable through a Program, a DeviceContext and a GPU. The rule:
//
//   * the arena is `candidates + 1 + kSearchTargetBudget + terminal reserve` target nodes;
//   * the SEARCH may not spend the terminal reserve, because the calls that run
//     AFTER the search -- the incumbent root-maximal and the seal fallback -- must hand back a handle
//     and cannot degrade;
//   * reaching a bound is a SEARCH STOP for a search caller and an invariant failure for a terminal
//     one.
//
// WHY THIS EXISTS AT ALL. Before 2026-09-28 every caller of `intern_target` threw `std::length_error`
// when the arena filled, which the journal reports as
// `WORKER RECOVER: pressure target arena is full [target-count]` and the client sees as an HTTP 500
// on top of a worker recovery. It fired on the e2e's phase 1 on EVERY tree -- the pre-change control
// and both arms with the message-boundary change, at 4101/4101 and 4102/4102 respectively -- which is
// what made the suite unrunnable past phase 1. One condition had two treatments: `construction_target`
// returned `nullopt` on the same bound and stopped the search, while the other three sites died.

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ninfer::models::qwen3_5::detail::planning_detail {

// THE SEARCH'S OWN BUDGET, and it is TIED to the runtime planner's by construction:
// `ninfer::runtime::kPlannerTargetBudget` (`materialization_planner.h`) is the single literal, and this
// mirrors it under a `static_assert` in `tests/test_materialization_budget.cpp`. The tie matters because
// the arena is sized to hold the search's budget PLUS the terminal reserve, so the reserve is only
// unreachable while the planner's budget does not exceed this.
//
// THE SIZE WAS THE BUG, NOT THE EQUALITY. The arena was `candidates + 1 + 4096` -- the search's budget
// plus ONE spare -- so a search that spent its budget filled the arena completely and left the calls
// after it that single spare. Measured 2026-09-28 (`targets=4099/4099 (limit=4099) ... candidates=2
// terminal=1`); the node that overflowed came from `commit_expansion`, which bounded itself by the whole
// arena rather than the search limit (now fixed -- see `target_arena_limit`'s use in
// `pressure_planner.cpp`).
inline constexpr std::size_t kSearchTargetBudget = 4096;

// ROOM THE CALLS THAT CANNOT DEGRADE NEED, per candidate. The seal fallback's own comment is explicit
// that a retention loss must never become a request failure, so this room is not negotiable.
//
// SIZED BY OVERSIZING, NOT BY DERIVATION -- stated plainly because the first version of this comment
// claimed otherwise and the claim was wrong. It said the measured run "needed three" because the call
// sites are reached per candidate. That is not supported by the numbers: post-search terminal demand is
// at most ONE node (the seal fallback), and the run that failed (`targets=4099/4099 (limit=4099)
// candidates=2 terminal=1`) is better explained by `commit_expansion` spending the reserve -- it checked
// against the WHOLE arena rather than the search limit -- than by extra terminal calls. That hole is now
// closed (expansion is bounded by the search limit), so this reserve is a backstop rather than the
// mechanism. `candidates + 2` is kept because it is at least as large as any observed demand; it is not
// derived from the call sites. The identity targets are not in this count -- they are interned in
// candidate order before the search, and `candidates.size()` of the bound is theirs.
inline constexpr std::size_t kTerminalTargetSlack = 2;

[[nodiscard]] constexpr std::size_t terminal_target_reserve(std::size_t candidates) noexcept {
    return candidates + kTerminalTargetSlack;
}

// THE ARENA IS DELIBERATELY BIGGER THAN THE SEARCH'S BUDGET. If it were merely equal, the capacity
// would be the binding constraint -- and a capacity stop is a TRUNCATION, which is exactly what the
// search must not do when its own budget is what should end it. Sizing it this way makes the capacity
// bound a backstop rather than the ordinary way the search ends, and leaves the terminal calls room
// that no budget-respecting search can spend.
[[nodiscard]] constexpr std::size_t target_arena_maximum(std::size_t candidates) noexcept {
    return candidates + 1U + kSearchTargetBudget + terminal_target_reserve(candidates);
}

// The limit a caller may fill the arena TO. A terminal caller gets the whole arena: the reserve above
// exists precisely so that it never has to be refused.
[[nodiscard]] constexpr std::size_t target_arena_limit(std::size_t candidates,
                                                       bool terminal) noexcept {
    return terminal ? target_arena_maximum(candidates)
                    : target_arena_maximum(candidates) - terminal_target_reserve(candidates);
}

// The victim-choice room a search call must leave for the terminal calls. Each new node stores one
// choice per owner, so the reserve is counted in the same unit. Without this the choice arena would
// be a SECOND way for a terminal call to be refused, and the target-count reserve would not cover it.
[[nodiscard]] constexpr std::size_t target_choice_reserve(std::size_t candidates,
                                                          std::size_t owners,
                                                          bool terminal) noexcept {
    return terminal ? 0U : terminal_target_reserve(candidates) * owners;
}

struct TargetArenaState {
    std::size_t targets           = 0;
    std::size_t target_capacity   = 0;
    std::size_t choices_used      = 0;
    std::size_t choices_capacity  = 0;
    std::size_t incoming_choices  = 0;
    std::size_t candidates        = 0;
    std::size_t owners            = 0;
    bool        terminal          = false;
};

enum class TargetArenaVerdict : std::uint8_t {
    Room = 0,
    TargetBound,     // the target nodes are at (or past) the limit
    ChoiceBound,     // the incoming choice set does not fit, or would eat the terminal reserve
    OrdinalOverflow, // a uint32 cast guard; unreachable while the two size_t bounds hold
};

[[nodiscard]] constexpr TargetArenaVerdict
target_arena_verdict(const TargetArenaState& state) noexcept {
    if (state.choices_capacity < state.choices_used) { return TargetArenaVerdict::OrdinalOverflow; }
    const std::size_t remaining = state.choices_capacity - state.choices_used;
    if (state.incoming_choices > std::numeric_limits<std::uint32_t>::max() ||
        state.choices_used > std::numeric_limits<std::uint32_t>::max()) {
        return TargetArenaVerdict::OrdinalOverflow;
    }
    const bool target_bound = state.targets >= target_arena_limit(state.candidates, state.terminal) ||
                              state.targets >= state.target_capacity;
    // THE INCOMING SET COUNTS AGAINST THE RESERVE. `remaining < reserve` alone let one search insert
    // consume part of the room the terminal calls need -- it refused the search only once the arena was
    // already below the reserve, i.e. after the fact. Masked until now by the arena being oversized: the
    // arithmetic is what makes it correct rather than lucky.
    const bool choice_bound =
        state.incoming_choices + target_choice_reserve(state.candidates, state.owners, state.terminal) >
        remaining;
    if (target_bound) { return TargetArenaVerdict::TargetBound; }
    if (choice_bound) { return TargetArenaVerdict::ChoiceBound; }
    return TargetArenaVerdict::Room;
}

} // namespace ninfer::models::qwen3_5::detail::planning_detail
