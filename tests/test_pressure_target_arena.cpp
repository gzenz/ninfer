// THE PRESSURE TARGET ARENA'S BOUND, decided without a Program, a DeviceContext or a GPU.
//
// This is the rule that failed on 2026-09-28: the arena filled and the request died as an HTTP 500
// plus a worker recovery, on every tree (the pre-change control at 4101/4101 and both arms with the
// message-boundary change at 4102/4102), which made the e2e's phase 1 unreachable. The rule is now a
// value a test can drive, so the next change to it fails here instead of on a live request.

#include "models/qwen3_5/program/planning/pressure_target_arena.h"

#include <iostream>

namespace arena = ninfer::models::qwen3_5::detail::planning_detail;

int main() {
    int failed = 0;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            std::cerr << "FAIL: " << what << '\n';
            ++failed;
        }
    };

    // THE RESERVE IS REAL. A search caller is held below the unreserved maximum, so the terminal
    // calls that run after it always have room. `candidates` is 4 here, matching the control's run.
    {
        constexpr std::size_t candidates = 4;
        constexpr std::size_t owners     = 3;
        const std::size_t maximum        = arena::target_arena_maximum(candidates);
        // THE PROPERTY THE FIRST FIX MISSED: the arena must be STRICTLY BIGGER than what a
        // budget-respecting search can spend. It was `candidates + 1 + 4096` -- exactly the search's
        // budget plus one spare -- so a search that spent its budget filled the arena completely and
        // the calls after it had the single spare. Measured: 4099/4099 with candidates=2, and a third
        // terminal call found nothing. A MUTANT THAT RESTORES THE EQUALITY MUST FAIL HERE.
        check(maximum > candidates + 1 + arena::kSearchTargetBudget,
              "MUTATION-SENSITIVE: the arena must hold MORE than the search's budget, or a search that "
              "spends its budget fills it and the terminal calls get only the spare");
        check(maximum == candidates + 1 + arena::kSearchTargetBudget +
                               arena::terminal_target_reserve(candidates),
              "the arena is the search's budget plus the terminal reserve plus one spare");
        check(arena::terminal_target_reserve(candidates) == candidates + arena::kTerminalTargetSlack,
              "the terminal reserve is derived from the candidate count, not a fixed count -- a fixed 2 "
              "was already wrong once (see the rescues' comment in pressure_target_arena.h for what the "
              "measured run actually shows, which is NOT the 'three terminal calls' first claimed)");
        check(arena::target_arena_limit(candidates, false) == maximum - arena::terminal_target_reserve(candidates),
              "a search caller's limit leaves the terminal reserve unspent");
        check(arena::target_arena_limit(candidates, true) == maximum,
              "a terminal caller may fill the whole arena -- the reserve exists so it never needs to be "
              "refused");

        // MUTATION-SENSITIVE: the search must be refused at its limit and the terminal call must not.
        // A verdict that ignored `terminal` would refuse both, which is the old behaviour for the
        // search and the old fatal path for the terminal call.
        const auto state_at = [&](std::size_t targets, bool terminal) {
            return arena::TargetArenaState{
                .targets           = targets,
                .target_capacity   = maximum,
                .choices_used      = 0,
                .choices_capacity  = 1'000'000,
                .incoming_choices  = 1,
                .candidates        = candidates,
                .owners            = owners,
                .terminal          = terminal,
            };
        };
        check(arena::target_arena_verdict(state_at(maximum - arena::terminal_target_reserve(candidates) - 1, false)) ==
                  arena::TargetArenaVerdict::Room,
              "a search caller one below its limit still has room");
        check(arena::target_arena_verdict(state_at(maximum - arena::terminal_target_reserve(candidates), false)) ==
                  arena::TargetArenaVerdict::TargetBound,
              "MUTATION-SENSITIVE: a search caller AT its limit must be refused -- this is the stop "
              "that replaces the throw");
        check(arena::target_arena_verdict(state_at(maximum - arena::terminal_target_reserve(candidates), true)) ==
                  arena::TargetArenaVerdict::Room,
              "MUTATION-SENSITIVE: a terminal caller at the same point must still have room, or the "
              "reserve buys nothing and root-maximal throws again");
        check(arena::target_arena_verdict(state_at(maximum, true)) ==
                  arena::TargetArenaVerdict::TargetBound,
              "even a terminal caller is bounded by the arena itself");
    }

    // THE CHOICE ARENA IS A SECOND BOUND, AND THE RESERVE COVERS IT IN THE SAME UNIT. Each new node
    // stores one choice per owner, so leaving two nodes of room means leaving `reserve * owners`
    // choices. Without this the target-count reserve would be defeated by the other arena -- which was
    // also nearly full in the measured failure (`choice_arena=11997/12420`).
    {
        constexpr std::size_t candidates = 4;
        constexpr std::size_t owners     = 3;
        const auto state = [&](std::size_t used, std::size_t incoming, bool terminal) {
            return arena::TargetArenaState{
                .targets           = 0,
                .target_capacity   = arena::target_arena_maximum(candidates),
                .choices_used      = used,
                .choices_capacity  = 12'420,
                .incoming_choices  = incoming,
                .candidates        = candidates,
                .owners            = owners,
                .terminal          = terminal,
            };
        };
        const std::size_t reserve = arena::target_choice_reserve(candidates, owners, false);
        check(reserve == arena::terminal_target_reserve(candidates) * owners,
              "the choice reserve is the node reserve expressed in choices");
        // THE INCOMING SET COUNTS: one choice below the reserve is still Room (it lands exactly ON it),
        // one choice more is refused. The first version of this check was `remaining < reserve`, which
        // allowed the insert and only refused once the arena was already past the reserve -- a real hole,
        // masked by the arena being oversized.
        check(arena::target_arena_verdict(state(12'420 - reserve - 1, 1, false)) ==
                  arena::TargetArenaVerdict::Room,
              "a search caller that lands exactly ON the choice reserve still has room");
        check(arena::target_arena_verdict(state(12'420 - reserve, 1, false)) ==
                  arena::TargetArenaVerdict::ChoiceBound,
              "MUTATION-SENSITIVE: a search insert that would eat ONE choice of the terminal calls' "
              "reserved room must be refused, even though the choice arena is not full");
        check(arena::target_arena_verdict(state(12'420 - reserve, 1, true)) ==
                  arena::TargetArenaVerdict::Room,
              "the same point is fine for a terminal caller");
        check(arena::target_arena_verdict(state(12'420, 1, true)) ==
                  arena::TargetArenaVerdict::ChoiceBound,
              "a full choice arena refuses everyone");
        check(arena::target_choice_reserve(candidates, owners, true) == 0,
              "a terminal caller reserves nothing -- it is the one the reserve is for");
    }

    // THE TARGET BOUND IS REPORTED BEFORE THE CHOICE BOUND when both hold, so the journal's `which`
    // names the bound the search actually reached rather than the last one tested.
    {
        const arena::TargetArenaState both{
            .targets           = 9'999,
            .target_capacity   = 4'101,
            .choices_used      = 12'420,
            .choices_capacity  = 12'420,
            .incoming_choices  = 1,
            .candidates        = 4,
            .owners            = 3,
            .terminal          = false,
        };
        check(arena::target_arena_verdict(both) == arena::TargetArenaVerdict::TargetBound,
              "with both bounds reached the target bound is the one named");
    }

    // A capacity smaller than the usage is an accounting break, not a bound -- and it must not be
    // reported as `Room`, which would let a resize bug through as a successful search.
    {
        const arena::TargetArenaState broken{
            .targets           = 0,
            .target_capacity   = 4'101,
            .choices_used      = 5,
            .choices_capacity  = 4,
            .incoming_choices  = 0,
            .candidates        = 4,
            .owners            = 3,
            .terminal          = true,
        };
        check(arena::target_arena_verdict(broken) == arena::TargetArenaVerdict::OrdinalOverflow,
              "usage past capacity is an accounting break, never Room");
    }

    if (failed == 0) { std::cout << "pressure target arena ok\n"; }
    return failed == 0 ? 0 : 1;
}
