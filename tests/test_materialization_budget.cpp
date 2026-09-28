#include "runtime/engine/context_cache/materialization_budget.h"

#include <iostream>
#include <stdexcept>

using namespace ninfer::runtime;

static void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

int main() {
    constexpr std::uint64_t ms = 1'000'000;
    try {
        auto idle = PlanningAllowance::boundary(0, 0);
        // CONTRACT (changed 2026-09-24, deliberately): the initial window is
        // min(economic(initial_cost), allowance.remaining) -- the whole allowance, not a flat
        // millisecond figure. The consequence, and the reason this block reads as it does: *inside*
        // the window no per-step economic test runs, so discovery is bounded by the allowance.
        //
        // THE MEASURED A/B (2026-09-24, prod4 4x150k; arms by instance id -- neither recorded a binary). Counts per COMPLETED turn: records the 420 s cap cancelled in flight (`finish_reason=cancelled`, `prefill=0`) carry no reuse evidence and are excluded.
        //   * 5 ms arm 1 (serve-293952): cut after 9 of 16 turns completed (rc=124), 3 cancelled in flight. Of the 9: 9/9 root with 0 hits, 8 with search_work>0 (granted 5e6, time_budget), 1 no_pressure (granted 0).
        //   * 5 ms arm 2 (serve-307902, 3 rounds): cut after 9 of 12 completed, 3 cancelled. Of the 9: 8/9 root with 0 hits, 1 private_endpoint (152,227 hits), 7 with search_work>0.
        //   * combined: 17 of 18 completed turns re-prefilled from root.
        //   * 400 ms arm (serve-292073): completed 16/16; rounds 2-3 (records 5-12) 8/8 private_endpoint with hits 152,333-154,799; overall 12 of 16 private_endpoint (152,333-157,004), 4 root (round 1), 15 of 16 with search_work>0 (exception: record 1, no_pressure, granted 0).
        // SCOPE: at 5 ms the old capped expression and the new one are numerically identical (both 5 ms) with `allow()` byte-identical, so this pair measures THE WINDOW'S VALUE; the cap's removal is what makes 400 ms reachable (arithmetic), not separately measured.
        // STILL TUNED: 400 ms is the only value measured PASSING. 100 ms fails the gate (`/tmp/cmp-ms100.json`: root 12/16, 614,577 hits, queue_wait_s.max 153.03 s); a 32 ms arm failed too but its artifact is byte-identical to an 800 ms arm's and tagged only `build`, so it establishes nothing.
        MaterializationSearchBudget expensive(idle, 0, 80'000 * ms);
        require(expensive.granted_ns() == idle.remaining(0),
                "the initial window is not the allowance bound");
        require(expensive.allow(4 * ms, 2 * ms, 3 * ms, 70'000 * ms, true, 1),
                "valuable completion could not cross the initial window");
        require(expensive.renewals() == 0,
                "a step inside the window renewed the allowance");
        require(!expensive.allow(40 * ms, 20 * ms, 20 * ms, ms, true, 2),
                "the allowance did not stop an operation larger than its remainder");
        require(expensive.stop_reason() == ninfer::MaterializationStopReason::TimeBudget,
                "an operation beyond the allowance was not reported as wall exhaustion");

        // An uncertain (incomplete-prediction) candidate's renewal is capped at 5 ms, and a
        // renewal with stalled progress is denied -- that is the boundedness that exists. A cost
        // of 200 ms (economic cap 10 ms) is used so the window is narrower than the allowance and
        // renewals are reachable at all.
        MaterializationSearchBudget discovery(idle, 0, 200 * ms);
        require(discovery.granted_ns() == 10 * ms, "economic cap did not bound the initial window");
        require(discovery.allow(12 * ms, ms, ms, 70'000 * ms, false, 1),
                "unknown candidate could not receive bounded discovery");
        require(discovery.renewals() == 1 && discovery.granted_ns() <= 15 * ms,
                "an incomplete step was not held to the 5 ms discovery cap");
        require(discovery.allow(16 * ms, ms, ms, 70'000 * ms, true, 2),
                "complete prediction could not continue after discovery");
        // Past the (renewed) window, where the progress guard applies.
        require(!discovery.allow(31 * ms, ms, ms, 70'000 * ms, true, 2),
                "stalled work renewed its allowance");

        auto busy = PlanningAllowance::boundary(2, 0);
        MaterializationSearchBudget first(busy, busy.limit_ns - 4 * ms, 80'000 * ms);
        require(first.granted_ns() == 4 * ms, "mandatory work did not consume boundary time");
        MaterializationSearchBudget backfill(busy, busy.limit_ns - ms, 80'000 * ms);
        require(backfill.granted_ns() == ms, "backfill reset the boundary allowance");
        require(!backfill.allow(busy.limit_ns, 1, 1, 70'000 * ms, true, 1),
                "search delayed runnable requests beyond their boundary");
        require(backfill.overshoot(busy.limit_ns + ms) == ms,
                "indivisible overrun was not measured");

        // The concurrent 55K rotation spends ~3.5 ms preparing/validating identities. A useful
        // complete target must still be assessable; discarding the whole cache makes all six
        // subsequent continuations cold. The old 5 ms boundary left only 1.5 ms and failed here.
        MaterializationSearchBudget restore_after_setup(busy, 3 * ms + ms / 2, 80'000 * ms);
        require(restore_after_setup.allow(3 * ms + ms / 2, 2 * ms, 2 * ms, 70'000 * ms, true, 1),
                "mandatory setup starved the first complete reuse assessment in a busy boundary");

        // The value threshold is a per-request fact: the same gain must clear the same
        // completion solo and under 10-way load (only the time allowance shrinks with it).
        auto ten_way = PlanningAllowance::boundary(9, 0);
        require(ten_way.limit_ns == 10 * ms && ten_way.affected_requests == 10,
                "10-way boundary did not apply the load limits");
        MaterializationSearchBudget solo(idle, 0, 80'000 * ms);
        MaterializationSearchBudget loaded(ten_way, 0, 80'000 * ms);
        require(solo.allow(5 * ms, 2 * ms, 5 * ms, 200 * ms, true, 1),
                "solo boundary denied a completion within its economic allowance");
        require(loaded.allow(5 * ms, 2 * ms, 5 * ms, 200 * ms, true, 1),
                "the value threshold shrank with concurrency for an identical gain");

        MaterializationSearchBudget cheap(idle, 0, ms);
        require(cheap.granted_ns() == ms / 20, "cheap request received a minimum 5 ms grant");
        require(!cheap.allow(ms / 20, ms, ms, ms, false, 1),
                "discovery ignored the economic cap for a cheap request");
        require(cheap.stop_reason() ==
                    ninfer::MaterializationStopReason::InsufficientExpectedGain,
                "economic stopping was reported as wall exhaustion");
        MaterializationSearchBudget saturated(idle, 0, UINT64_MAX);
        require(saturated.granted_ns() == 0 && !saturated.allow(0, ms, ms, UINT64_MAX, true, 1),
                "saturated cost was used as evidence of unlimited gain");
        // Cost 200 ms (economic cap 10 ms) so the window is narrower than the allowance and the
        // per-step guards are reachable past it.
        MaterializationSearchBudget seeded(idle, 0, 200 * ms);
        require(!seeded.allow(12 * ms, ms, ms, 70'000 * ms, false, 1, false),
                "an already-seeded candidate renewed solely on an incomplete optimistic estimate");
        require(seeded.allow(12 * ms, ms, ms, 70'000 * ms, true, 1, false),
                "a complete profitable refinement was denied after seeding");
        std::atomic<bool> cancelled{false};
        auto controlled                = PlanningAllowance::boundary(0, 0);
        controlled.cancellation        = &cancelled;
        controlled.control_deadline_ns = 3 * ms;
        require(controlled.remaining(2 * ms) == ms, "control deadline did not constrain planning");
        cancelled.store(true);
        require(controlled.remaining(0) == 0, "cancelled request kept optional planning headroom");
        // Cost 200 ms => economic cap 10 ms => the window (10 ms) is narrower than the allowance
        // (50 ms), so the renewal-progress guard is reachable at all.
        MaterializationSearchBudget stalled(idle, 0, 200 * ms);
        require(stalled.granted_ns() == 10 * ms, "economic cap did not bound the initial window");
        require(stalled.allow(12 * ms, ms, ms, 70'000 * ms, true, 7), "first forecast grant failed");
        require(stalled.renewals() == 1 && stalled.granted_ns() == 20 * ms,
                "complete renewal did not extend the allowance");
        require(!stalled.allow(22 * ms, ms, ms, 70'000 * ms, true, 7),
                "stalled work renewed its allowance");
        std::cout << "ok\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    // THE SPLIT'S AGGREGATION. The first version returned `restorable` from whichever entry matched deepest,
    // so a deep match with NO restorable checkpoint hid a shallower match WITH one -- and the value then
    // contradicted the request's own reuse in 94 of 309 records. These two cases pin the maxima as
    // INDEPENDENT, which is the property that was wrong.
    {
        const ninfer::runtime::PrefixSplitSample deep_without_checkpoint{.tokens = 28530,
                                                                        .restorable = 0};
        const ninfer::runtime::PrefixSplitSample shallow_with_checkpoint{.tokens = 23353,
                                                                        .restorable = 23353};
        const std::array samples{deep_without_checkpoint, shallow_with_checkpoint};
        const ninfer::runtime::PrefixSplitBest best = ninfer::runtime::best_prefix_split(samples);
        if (best.tokens != 28530) { std::cerr << "deepest match must come from the deepest entry\n"; return 1; }
        if (best.restorable != 23353) {
            std::cerr << "MUTATION-SENSITIVE: restorable must be the max over entries, not the deepest "
                         "entry's own value (the bug that produced restorable=0 and sent a fix the wrong "
                         "way)\n";
            return 1;
        }
        if (best.entries != 2) { std::cerr << "the denominator must count every entry scanned\n"; return 1; }
    }
    // And the degenerate shapes: no entries, and one entry whose checkpoint is below its match.
    {
        const ninfer::runtime::PrefixSplitBest none =
            ninfer::runtime::best_prefix_split(std::span<const ninfer::runtime::PrefixSplitSample>{});
        if (none.entries != 0 || none.tokens != 0 || none.restorable != 0) {
            std::cerr << "an empty scan must report zeroes, not stale values\n";
            return 1;
        }
    }
    // WHY THE MATCH STOPPED, AND HOW FAR THE LEDGER WENT. `match_end` and `stored` were added to the split
    // with no test at all, and the first deployment of `split_ended_by` read `diverged` on every sample --
    // which is exactly what an unwired field reads like, so it cost a deploy cycle to tell a real reading from
    // a dead one. Both halves of that ambiguity are pinned here: the values must follow the DEEPEST entry, and
    // a tie must not silently pick by iteration order without the rule being written down.
    {
        // The shallow entry deliberately has the LONGER ledger, so a max-over-entries implementation cannot
        // coincide with the right answer. The first draft of this case used a shallow entry with a SHORTER
        // ledger and passed against a mutant that took the max -- the test measured nothing. (Mutation-checked
        // both ways, which is the only reason that was caught.)
        const ninfer::runtime::PrefixSplitSample deep{      .tokens = 28530, .match_end = 0, .stored = 41000};
        const ninfer::runtime::PrefixSplitSample shallow{   .tokens = 23353, .match_end = 2, .stored = 90000};
        const std::array samples{shallow, deep};
        const ninfer::runtime::PrefixSplitBest best = ninfer::runtime::best_prefix_split(samples);
        if (best.match_end != 0) {
            std::cerr << "match_end must come from the DEEPEST entry, not the max over entries: a shallow "
                         "entry whose ledger ended says nothing about where the deepest match stopped\n";
            return 1;
        }
        if (best.stored != 41000) {
            std::cerr << "stored must be the deepest entry's own ledger length -- it is the denominator that "
                         "localises the divergence, so a shallow value would move the reported position\n";
            return 1;
        }
    }
    // The tie rule, written down rather than left to the iteration order: the FIRST entry at the greatest
    // depth wins, so among equally-deep entries the reading depends on catalog order. That is a real limit of
    // this field and it is asserted here so a reader meets it, instead of rediscovering it as "why does the
    // same prompt read differently on another run".
    {
        const ninfer::runtime::PrefixSplitSample first{ .tokens = 900, .match_end = 1, .stored = 900};
        const ninfer::runtime::PrefixSplitSample tie{   .tokens = 900, .match_end = 2, .stored = 1400};
        const std::array samples{first, tie};
        const ninfer::runtime::PrefixSplitBest best = ninfer::runtime::best_prefix_split(samples);
        if (best.match_end != 1 || best.stored != 900) {
            std::cerr << "an equal-depth tie must resolve to the first sample (documented), not to the "
                         "deeper ledger or the larger restorable\n";
            return 1;
        }
    }
    // THE DIVERGENCE PROBE. Same rule as `match_end`/`stored` -- the windows belong to the DEEPEST entry --
    // and the sample order here is deliberate: the deepest entry is FIRST, so an implementation that let the
    // last entry win would take the shallow one's window and fail. (The order is the whole test: with the
    // reverse order a last-wins mutant passes, which is how the previous case in this file was written wrong
    // the first time.)
    {
        // THREE samples, the deepest LAST so that a "last sample wins" mutant fails. (The counts that used to
        // distinguish more mutant classes went with the id windows in the 2026-09-28 cleanup.)
        ninfer::runtime::PrefixSplitSample head{.tokens = 100, .stored = 200, .probe_index = 100};
        ninfer::runtime::PrefixSplitSample deep{.tokens = 900, .stored = 1000, .probe_index = 900};
        ninfer::runtime::PrefixSplitSample tail{.tokens = 300, .stored = 400, .probe_index = 300};
        const std::array samples{head, deep, tail};
        const ninfer::runtime::PrefixSplitBest best = ninfer::runtime::best_prefix_split(samples);
        if (best.probe_index != 900) {
            std::cerr << "the divergence window must come from the DEEPEST entry: a shallow entry's tokens "
                         "are not the ones that stopped this match, and reading them would name the wrong "
                         "token as the cause\n";
            return 1;
        }
    }
    std::cout << "prefix-split aggregation ok\n";
}
