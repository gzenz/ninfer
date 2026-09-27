// #6's ordering, asserted on the struct the planner actually compares (not a copy of it).
//
// The operator's ruling is "evicting a victim that holds a restorable checkpoint while the host has room is a
// defect", and the whole of the fix is the ORDER of `FoldedCost::key()`: `restorable_evictions` first, ahead
// of `total_ns`. Before it, the fold priced a demote (host bytes + transfers) against an eviction (free) and
// cost decided first, so a merely cheaper plan won before the destruction count was consulted -- which is
// how prod evicted 65k-76k-token frontiers holding an endpoint or rewrite checkpoint with 5-7 host state
// slots and ~23 GB of host KV free (2026-09-26 18:13).
//
// The positive control this change otherwise lacks: the battery is byte-identical with and without it,
// because no scenario in it contains a restorable-checkpoint eviction, so nothing else can fail when the
// ordering is wrong. Mutation-check: swap the first two elements of `key()` and case 1 and case 3 below
// must fail.

#include "runtime/engine/context_cache/materialization_budget.h"

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* name) {
    if (condition) {
        std::printf("ok   %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

ninfer::runtime::FoldedCost cost(std::uint64_t total_ns, std::uint32_t restorable_evictions) {
    ninfer::runtime::FoldedCost value;
    value.total_ns             = total_ns;
    value.restorable_evictions = restorable_evictions;
    return value;
}

}  // namespace

int main() {
    // 1. PRESERVATION OUTRANKS COST. Nine times the price, one restorable eviction fewer: preferred.
    const auto cheap_evicting    = cost(100, 1);
    const auto costly_preserving = cost(900, 0);
    check(costly_preserving.less(cheap_evicting),
          "a costlier plan that evicts no restorable victim is preferred");
    check(!cheap_evicting.less(costly_preserving),
          "and the cheaper evicting plan is NOT preferred -- this is the defect the ruling names");

    // 2. With preservation equal, COST still decides.
    check(cost(100, 0).less(cost(200, 0)), "with equal preservation, the cheaper plan wins");
    check(!cost(200, 0).less(cost(100, 0)), "and the ordering is strict, not reversed");

    // 3. The count is the PRIMARY key, not a tiebreak: no amount of cost buys a restorable eviction.
    const auto one_eviction_cheap     = cost(1, 1);
    const auto zero_evictions_astronomical = cost(1ULL << 40U, 0);
    check(zero_evictions_astronomical.less(one_eviction_cheap),
          "even a millionfold cost cannot buy one restorable eviction");

    // 4. ONLY restorable evictions are dominant. `owner_evictions` and `checkpoint_drops` alone must NOT
    // outrank cost: promoting them did exactly that and REGRESSED `replacement private long anchor was not
    // reusable` (rc=1), because preserving a checkpoint that is not restorable is not what the ruling asks.
    ninfer::runtime::FoldedCost many_plain_evictions_cheap;
    many_plain_evictions_cheap.total_ns         = 100;
    many_plain_evictions_cheap.owner_evictions  = 9;
    many_plain_evictions_cheap.checkpoint_drops = 9;
    const auto none_plain_evictions_expensive = cost(200, 0);
    check(many_plain_evictions_cheap.less(none_plain_evictions_expensive),
          "a plain eviction count does NOT dominate cost -- only RESTORABLE evictions do");

    // 5. A restorable eviction is still worse than a plain one at equal cost: the two are distinct fields.
    ninfer::runtime::FoldedCost plain_eviction;
    plain_eviction.total_ns        = 100;
    plain_eviction.owner_evictions = 1;
    check(plain_eviction.less(cost(100, 1)),
          "at equal cost, evictions that DROP nothing are preferred to ones that do");

    if (failures != 0) {
        std::printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("all preservation-ordering checks passed\n");
    return 0;
}
