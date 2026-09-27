#pragma once

#include "ninfer/types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ninfer::runtime {

// THE PUBLICATION-CELL LOSS, at namespace scope for the same reason as the cost key below: a host-only test
// can decide the case without a GPU.
//
// WHY THIS IS NOT "A REFUSAL COUNTER", WHICH IS WHAT THE FIRST VERSION OF IT WAS AND WHY THAT WAS WRONG.
// The planner probes a cell-free option from FIVE call sites (`materialization_planner.h:163, 297, 471,
// 550, 590`) -- not once per assessed target: `rank_guidance` alone runs twice per candidate in
// `start_path` and again for every construction option and every chosen target. Counting those calls
// therefore counts SEARCH STEPS. Measured: 108,544 of them across 24 requests on one run, and the same
// instrument printed 97 of them on another run in which four requests achieved 99.9% turn-closure reuse.
// A number that goes nonzero whenever retention fills the catalog -- which is the normal state under
// eviction-to-publish -- cannot falsify a capacity raise, and as an alert it fires during healthy
// operation. So the event is decided HERE instead, once per planning run, and it means something narrow:
//
//   a candidate whose EVERY goal probe failed on the publication cell -- never on any other condition --
//   could not be adopted at all, and would have reused strictly more tokens than the plan that won.
//
// That is the only shape in which the catalog has actually cost reuse.
struct PublicationCellProbe {
    // Every probe made for this candidate, with the reason a failed one failed. `cell_only` and `other` are
    // the two ways to fail and they are exclusive; a candidate with `goals != 0` could be adopted.
    std::uint32_t probes    = 0;
    std::uint32_t cell_only = 0;
    std::uint32_t other     = 0;
    std::uint32_t goals     = 0;
};

struct PublicationCellLoss {
    std::uint32_t candidates         = 0;  // candidates the cell alone made unadoptable
    std::uint32_t best_blocked_reuse = 0;  // the most tokens any of them would have reused
    // THE AT-RISK BREAKDOWN, and it exists to settle a disagreement that reading the code did not. A review
    // pass argued that a catalog-caused loss cannot occur at prod's shape (C=32 cells, L=4 lanes) because a
    // cell-only failure needs no free cell AND no evictable owner, and the count of non-evictable owned cells
    // is bounded by the lane count -- so it needs roughly `C <= 2L + Claimed`. If that is right, this
    // instrument can never fire on prod for the reason it claims, and its only firing would be search
    // truncation. The counter-argument is that an owner being EVICTABLE is not the same as an owner this
    // candidate's own solution EVICTS, and a preserving (zero-eviction) candidate gets no cell from victims
    // however many owners are evictable.
    //
    // `at_risk` (non-winner candidates with any cell-only failure, whatever vetoed them) against
    // `evictable_owners` (was there a private owner record at all) decides it: if at_risk > 0 always comes
    // with evictable_owners > 0, the review is right and the predicate must be rebuilt from catalog state. If
    // at_risk > 0 with evictable_owners == 0 occurs, the cell really was unobtainable and the mechanism is
    // real. NOT counted by `candidates`: a candidate can be at risk and still not count (it was vetoed).
    std::uint32_t at_risk            = 0;
    std::uint32_t blocked_by_goals   = 0;  // at-risk candidates a successful goal also existed for
    std::uint32_t blocked_by_other   = 0;  // at-risk candidates that also failed on something else
    std::uint32_t blocked_by_reuse   = 0;  // at-risk candidates that would not have out-reused the winner
};

// `reuse_of(i)` is the tokens candidate `i` would have reused. Excluded: the winner itself (its own probes
// failing on the cell is the eviction-to-publish path, which is expected and is not a loss) and any
// candidate that could have been adopted or that failed for a reason other than the cell.
template <typename ReuseOf>
[[nodiscard]] PublicationCellLoss publication_cell_loss(std::span<const PublicationCellProbe> probes,
                                                       std::size_t candidate_count,
                                                       std::size_t winner_index,
                                                       std::uint32_t winner_reuse, ReuseOf&& reuse_of) {
    PublicationCellLoss loss;
    const std::size_t bounded = std::min(candidate_count, probes.size());
    for (std::size_t index = 0; index < bounded; ++index) {
        const PublicationCellProbe& probe = probes[index];
        // `probe.probes == 0U` USED to be the first conjunct and is deleted on purpose: every tallied probe
        // increments exactly one of `goals`/`cell_only`/`other`, so `probes == goals + cell_only + other`
        // always holds and `cell_only > 0` already implies `probes > 0`. A mutant that removes it passed all
        // thirteen checks (verified 2026-09-27) -- an untestable conjunct is not a defensive one, and keeping
        // it would invite a reader to think the classification can be skipped. If a future caller counts
        // probes WITHOUT classifying them, the fix is that caller, not a guard here.
        if (probe.cell_only == 0U || index == winner_index) { continue; }
        // AT RISK: the cell blocked this candidate at least once. Whether any single veto is enough to
        // disqualify it is the next question, and the breakdown says which veto did.
        ++loss.at_risk;
        if (probe.goals != 0U) { ++loss.blocked_by_goals; continue; }
        if (probe.other != 0U) { ++loss.blocked_by_other; continue; }
        const std::uint32_t reuse = static_cast<std::uint32_t>(reuse_of(index));
        if (reuse <= winner_reuse) { ++loss.blocked_by_reuse; continue; }
        ++loss.candidates;
        loss.best_blocked_reuse = std::max(loss.best_blocked_reuse, reuse);
    }
    return loss;
}

// THE SPLIT'S AGGREGATION, at namespace scope so a host-only test can drive it -- because the first version
// of this was WRONG in a way no test would have caught, and it sent a fix the wrong way.
//
// What it did: it reported `restorable` from whichever entry matched DEEPEST, so an entry matching 28,530 with
// no restorable checkpoint hid a second entry matching 23,353 WITH one. The field then contradicted the
// request's own `chosen_reuse` in 94 of 309 records -- the request demonstrably resumed from a checkpoint the
// field said did not exist -- and the "no checkpoint below the match, so capture one" conclusion was built on
// that artifact.
//
// The two maxima are INDEPENDENT and that is the whole point: the deepest MATCH and the deepest RESTORABLE
// checkpoint need not come from the same entry, and a reader must not infer one from the other.
struct PrefixSplitSample {
    std::uint32_t tokens     = 0;  // this entry's token-exact match
    std::uint32_t restorable = 0;  // this entry's deepest restorable checkpoint at or below its own match
};

struct PrefixSplitBest {
    std::uint32_t tokens     = 0;
    std::uint32_t restorable = 0;
    std::uint32_t entries    = 0;
};

[[nodiscard]] inline PrefixSplitBest best_prefix_split(std::span<const PrefixSplitSample> samples) noexcept {
    PrefixSplitBest best;
    for (const PrefixSplitSample& sample : samples) {
        ++best.entries;
        best.tokens     = std::max(best.tokens, sample.tokens);
        best.restorable = std::max(best.restorable, sample.restorable);
    }
    return best;
}

// #6: THE INCUMBENT COST KEY, at namespace scope so a host-only test can compare two of them and assert
// the ordering directly. It was private to the planner, which is why nothing tested it -- and the ordering
// it encodes IS the operator's ruling ("evicting a victim that holds a restorable checkpoint while the host
// has room is a defect"): `restorable_evictions` is the FIRST element, ahead of `total_ns`, so among
// feasible plans fewer evictions of victims that held a recoverable checkpoint wins and cost decides among
// equals. A test asserting this on a COPY of the ordering would drift the first time either changed, so the
// planner compares this very struct.
struct FoldedCost {
    std::uint64_t now_ns                    = 0;
    std::uint64_t future_loss_ns            = 0;
    std::uint64_t total_ns                  = 0;
    std::uint64_t lower_bound_ns            = 0;
    std::uint64_t affected_selected_hits    = 0;
    std::uint64_t newest_affected_hit_epoch = 0;
    std::uint32_t owner_evictions           = 0;
    // #6: evictions of victims that held a RESTORABLE checkpoint -- the exact quantity the operator's
    // ruling names, and the ONLY one that outranks cost in `key()`. Kept separate from
    // `owner_evictions` because a victim with no checkpoint to lose costs nothing here, and from
    // `checkpoint_drops` because that counts every drop, restorable or not: a blanket ordering on
    // either one regressed `replacement private long anchor was not reusable` when it was tried.
    std::uint32_t restorable_evictions      = 0;
    std::uint32_t checkpoint_drops          = 0;
    std::uint32_t copy_operations           = 0;
    std::uint64_t transferred_bytes         = 0;
    std::uint64_t remaining_text_prefill    = 0;
    std::uint64_t remaining_vision_prefill  = 0;
    std::uint32_t reused_prompt_tokens      = 0;
    bool current_session_binding            = false;
    std::uint32_t candidate_ordinal         = 0;
    std::uint32_t target_ordinal            = 0;

    [[nodiscard]] auto key() const noexcept {
        // #6: PRESERVATION DOMINATES COST, and the ordering here is the whole of that decision.
        //
        // `checkpoint_drops` and `owner_evictions` used to sit FOURTH and FIFTH, behind `total_ns`, so
        // a plan that was merely cheaper won over one that kept a restorable checkpoint: the fold
        // prices a demote (host bytes + transfers) against an eviction (free), and cost decided before
        // the destruction count was ever consulted. The operator's ruling is that evicting a victim
        // holding a restorable checkpoint while the host has room is a DEFECT, not a trade, so the two
        // counts now come first: among feasible plans, fewer dropped checkpoints wins, then fewer
        // evictions, and only then cost.
        //
        // `restorable_evictions` is FIRST and everything else is where it was: the ruling names
        // evictions of victims HOLDING A RESTORABLE CHECKPOINT, and nothing wider. An earlier version
        // of this change promoted `checkpoint_drops` and `owner_evictions` too and regressed
        // `replacement private long anchor was not reusable` -- preserving a checkpoint the scenario
        // needs dropped (a non-restorable one) is not what the ruling asks for.
        return std::tuple{
            restorable_evictions,
            total_ns,
            affected_selected_hits,
            newest_affected_hit_epoch,
            owner_evictions,
            checkpoint_drops,
            copy_operations,
            transferred_bytes,
            remaining_text_prefill,
            remaining_vision_prefill,
            std::numeric_limits<std::uint32_t>::max() - reused_prompt_tokens,
            current_session_binding ? 0U : 1U,
            candidate_ordinal,
            target_ordinal,
        };
    }

    [[nodiscard]] bool less(const FoldedCost& other) const noexcept {
        return key() < other.key();
    }
};



template <class Clock = std::chrono::steady_clock>
[[nodiscard]] std::uint64_t planning_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

// One Engine admission boundary, including all head/backfill inspections. Correctness work and
// sealing cannot be cancelled by this allowance, but their elapsed time reduces optional headroom.
struct PlanningAllowance {
    std::uint64_t started_ns              = planning_now_ns();
    std::uint64_t limit_ns                = 50'000'000;
    std::uint32_t affected_requests       = 1;
    const std::atomic<bool>* cancellation = nullptr;
    std::uint64_t control_deadline_ns     = std::numeric_limits<std::uint64_t>::max();

    [[nodiscard]] static PlanningAllowance
    boundary(std::uint32_t other_runnable, std::uint64_t now = planning_now_ns()) noexcept {
        return {.started_ns        = now,
                .limit_ns          = other_runnable == 0 ? 50'000'000ULL : 10'000'000ULL,
                .affected_requests = 1U + other_runnable};
    }

    [[nodiscard]] std::uint64_t remaining(std::uint64_t now) const noexcept {
        if ((cancellation && cancellation->load(std::memory_order_relaxed)) ||
            now >= control_deadline_ns) {
            return 0;
        }
        const auto elapsed = now > started_ns ? now - started_ns : 0;
        return std::min(elapsed < limit_ns ? limit_ns - elapsed : 0, control_deadline_ns - now);
    }
};

// Integer timestamps make wall-budget decisions reproducible without sleeping in policy tests.
// Estimates control optional work only; they never certify feasibility or a search bound.
class MaterializationSearchBudget {
public:
    MaterializationSearchBudget(PlanningAllowance allowance, std::uint64_t started,
                                std::uint64_t initial_cost) noexcept
        : allowance_(allowance), started_(started),
          // No flat millisecond cap on the initial window. What the artifacts support, with the
          // denominators stated because this repo keeps paying for claims without them:
          //
          //  * the gate's own FORECAST for a construction step (`GATE DENY phase=construction
          //    completion=…` -- the `completion` argument handed to `allow_work`, an estimate, not a
          //    measured duration). The figure that motivated this change is from ONE log: `/tmp/why.log`
          //    (n=8, all `complete=0`), 7.4-30.8 ms. Across every surviving log the same line reads
          //    **0.024-30.8 ms (n=92, p50 4.7 ms, 71 of them `complete=1`)** -- so "several times the
          //    flat 5 ms window" is true of that log's construction steps and NOT of the population.
          //    The decision does not rest on it: the A/B below is the measurement;
          //  * the collapse artifacts' stop reasons read `{'no_pressure': 1, 'time_budget': 15}` with
          //    0% prefix reuse;
          //  * the 5 ms arm below shows a work unit WAS admitted inside a 5 ms window, so the tempting
          //    reading "the first step could not be admitted" is FALSE -- the window opened, one work
          //    unit ran, and the search was cut off before finding the candidate. What remains (the
          //    collapse) is labelled INFERENCE where it is stated.
          //
          // THE MEASURED A/B (2026-09-24, prod4 4x150k; arms by instance id in ~/ninfer-requests.jsonl -- neither arm recorded a binary identity). Counts are per COMPLETED turn: records the 420 s cap cancelled in flight carry `finish_reason=cancelled`, `prefill=0`, no reuse evidence, and are excluded -- an earlier version of this comment counted them as turns, which flattered the ratio and hid that a third of the "12 records" never ran.
          //   * 5 ms arm 1 (serve-293952): cut by the 420 s cap after 9 of 16 turns completed (rc=124); 3 further turns cancelled in flight. Of the 9 completed: 9/9 root with 0 hits, 8 with `search_work>0` (`granted=5e6`, `time_budget`), 1 `no_pressure` (`granted=0`).
          //   * 5 ms arm 2 (serve-307902, 3 rounds): cut after 9 of 12 completed, 3 cancelled. Of the 9: 8/9 root with 0 hits, 1 `private_endpoint` (152,227 hits); 7 with `search_work>0`.
          //   * combined: 17 of 18 completed turns re-prefilled from root.
          //   * 400 ms arm (serve-292073): completed 16/16. Rounds 2-3 (records 5-12): 8/8 `private_endpoint`, hits 152,333-154,799. Overall 12 of 16 `private_endpoint` (152,333-157,004) and 4 root (round 1, records 1-4); 15 of 16 with `search_work>0` -- the exception is record 1, `no_pressure`, `granted=0`.
          // SCOPE: at a 5 ms allowance the old capped expression and the new one are numerically identical (both 5 ms) and `allow()` is byte-identical, so this pair measures THE WINDOW'S VALUE, not the cap removal as such. The cap's contribution is arithmetic: kept, with the allowance at 400 ms, `granted_` would still be 5 ms -- so it is what makes the measured window reachable, and its effect is not separately measured.
          // STILL TUNED, not derived: 400 ms is the only value measured PASSING. 100 ms fails the gate (`/tmp/cmp-ms100.json`: root 12/16, 614,577 hits, `queue_wait_s.max` 153.03 s, 67% root in rounds 2+), and the no-reuse collapse is `/tmp/cmp-after.json` (`{no_pressure: 1, time_budget: 15}`, root 16/16, 0 hits -- its own `n_errors` is 0; the 22 turn errors belong to the narrowed-window arms, `cmp-ms800`/`cmp-w32`).
          // KNOWN COST, deliberate: inside this window the per-step economic test does not run (the
          // shortcut below precedes it), so discovery is bounded by the allowance rather than per
          // step. In the prod4 4x150k profile that is the whole budget -- `search_granted_ns` is
          // 4e8 on every record that searched (15 of 16; record 1 is `no_pressure`/`granted=0`), i.e.
          // `granted_ == remaining`, because `economic(initial_cost)` is far above the allowance: the
          // cost field (`initial_predicted_total_ns`) reads 1.239e11-8.798e12 ns across the prod4
          // instances and `economic` is that divided by 20 -- **6.2e9-4.4e11 ns**. (An earlier version
          // quoted the cost range as if it were the economic bound: 20x too large at the top end.)
          // Ordinary traffic is not the prod4 profile, where `granted_ == remaining` still holds;
          // across all records the smallest granted value on a record that actually searched
          // (`search_work>0`) is 4,270,230 ns (`serve-820`); one record with `search_work=0` was
          // granted 35,391 ns, and 901 records carry `granted=0` because no search was needed. No
          // upper bound is claimed here. The test reaches the per-step guards with a 200 ms cost.
          //
          // The 32 ms arm did not complete (1200 s cap, 22 turn errors, 0 prefix hits) and its result
          // file is byte-identical to an 800 ms arm's and tagged only `build`, so no arm's provenance
          // is established and none of it is cited as evidence here.
          granted_(std::min(economic(initial_cost), allowance.remaining(started))) {}

    [[nodiscard]] bool allow(std::uint64_t now, std::uint64_t next_operation_ns,
                             std::uint64_t completion_ns, std::uint64_t gain_ns,
                             bool complete_prediction, std::uint64_t progress,
                             bool discovery_eligible = true) noexcept {
        boundary_limited_             = false;
        const std::uint64_t elapsed   = now > started_ ? now - started_ : 0;
        const std::uint64_t remaining = allowance_.remaining(now);
        next_operation_ns             = std::max<std::uint64_t>(1, next_operation_ns);
        completion_ns                 = std::max(next_operation_ns, completion_ns);
        if (next_operation_ns > remaining) {
            boundary_limited_ = true;
            reason_           = MaterializationStopReason::TimeBudget;
            return false;
        }
        if (elapsed < granted_ && next_operation_ns <= granted_ - elapsed) { return true; }
        if (completion_ns > remaining || completion_ns > economic(gain_ns)) {
            reason_ = MaterializationStopReason::InsufficientExpectedGain;
            return false;
        }
        // An incomplete (uncertain) forecast is only admitted for a discovery-eligible node. The
        // eligibility is per-candidate (the caller passes it); cheap confirmation steps such as
        // the assessment of an already-generated preserving alternative are admitted so a demote
        // is not generated and then left unassessed.
        if (!complete_prediction && !discovery_eligible) {
            reason_ = MaterializationStopReason::InsufficientExpectedGain;
            return false;
        }
        if (renewals_ != 0 && progress <= renewal_progress_) {
            reason_ = MaterializationStopReason::InsufficientExpectedGain;
            return false;
        }
        const auto hard = elapsed + remaining; // bounded by the allowance's representable duration
        const auto growth = std::max(granted_, next_operation_ns);
        auto added        = std::min(growth, hard > granted_ ? hard - granted_ : 0);
        if (!complete_prediction) { added = std::min<std::uint64_t>(added, 5'000'000); }
        if (added == 0 || elapsed + next_operation_ns > granted_ + added) {
            reason_ = MaterializationStopReason::TimeBudget;
            return false;
        }
        granted_ += added;
        ++renewals_;
        renewal_progress_ = progress;
        discovery_used_   = discovery_used_ || !complete_prediction;
        return true;
    }

    [[nodiscard]] std::uint64_t granted_ns() const noexcept { return granted_; }

    [[nodiscard]] std::uint32_t renewals() const noexcept { return renewals_; }

    [[nodiscard]] bool boundary_limited() const noexcept { return boundary_limited_; }

    [[nodiscard]] bool discovery_used() const noexcept { return discovery_used_; }

    [[nodiscard]] MaterializationStopReason stop_reason() const noexcept { return reason_; }

    [[nodiscard]] std::uint64_t overshoot(std::uint64_t now) const noexcept {
        const auto elapsed = now > started_ ? now - started_ : 0;
        return elapsed > granted_ ? elapsed - granted_ : 0;
    }

private:
    // The gain is a per-request fact (the re-prefill a restore avoids). Dividing it by the
    // concurrency inverts the economics: under load re-prefills queue and cost more, so the
    // value threshold must not shrink as affected requests rise. The time allowance above is
    // the legitimate concurrency fairness bound.
    [[nodiscard]] std::uint64_t economic(std::uint64_t gain) const noexcept {
        if (gain == std::numeric_limits<std::uint64_t>::max()) { return 0; }
        return gain / 20U;
    }

    bool boundary_limited_ = false;
    PlanningAllowance allowance_;
    std::uint64_t started_            = 0;
    std::uint64_t granted_            = 0;
    std::uint64_t renewal_progress_   = 0;
    std::uint32_t renewals_           = 0;
    bool discovery_used_              = false;
    MaterializationStopReason reason_ = MaterializationStopReason::TimeBudget;
};

} // namespace ninfer::runtime
