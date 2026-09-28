// The private catalog's LOSS decision, asserted on the function the planner actually calls.
//
// WHAT THIS GUARDS, and why the first version of the instrument got it wrong. The event that matters is
// "this request could have reused more, and the publication cell is the only thing that stopped it". The
// first version counted the goal builder's cell-free PROBES instead: the planner makes those on its way to an
// ordinary eviction-to-publish (108,544 in one 24-request run; 97 PRINTED LINES -- about 45.6k probes, the
// print being capped -- in another
// run where four requests reused 99.9%), so the number went nonzero whenever retention filled the catalog.
// That cannot falsify a capacity raise, and as an alert it fires on healthy traffic.
//
// The case that separates the two is the WINNER: a winner's own probes fail on the cell as a matter of
// course, and must contribute ZERO. Case 1 below is that case, and it fails if the winner is not excluded.
//
// MUTATION-CHECKED, AND THE TABLE IS RE-MEASURED AGAINST THIS REVISION RATHER THAN INHERITED. Each mutant
// was compiled out of tree against a mutated copy of the header (control green first, 24 checks) and the
// failures recorded are the ACTUAL ones. The claim is not "one case each" -- most conjuncts are exercised from
// two or three angles now -- but the stronger and true one: **every mutant's failures are confined to cases
// that test the conjunct it broke, and none fails a case testing a different conjunct.**
//
//   m1  drop the winner exclusion      -> cases 1, 11            (the two winner cases)
//   m2  accept `other != 0`            -> cases 4, 10            (the other-failure cases)
//   m3  accept `goals != 0`            -> cases 5, 10, 12        (the goal cases)
//   m4  accept `reuse <= winner_reuse` -> cases 3, 10            (the reuse cases)
//   m5  drop `cell_only == 0`          -> cases 6b, 7, 11        (the unclassified/never-probed cases)
//   m10 count the winner in `at_risk`  -> cases 10, 11, 12        (the at-risk cases)
//   m11 swap the goals/other veto order-> case 12                (the case with both vetoes)
//   m7  drop the `std::min` bound      -> NOTHING unless built with -D_GLIBCXX_ASSERTIONS, under which it
//                                         aborts at span:288. The target sets that flag for exactly this
//                                         reason; without it case 9 cannot go red.
//
// Three corrections this table records rather than hides, because each was a wrong claim in an earlier
// revision: the mapping was originally measured BEFORE case 10 existed and was stale; m10 and m11 (the at-risk
// winner exclusion and the veto order) passed every check until cases 11 and 12 were added for them; and
// `probes == 0` was deleted as a conjunct because a mutant removing it passed everything -- it is implied by
// `cell_only > 0`, since every tallied probe increments exactly one class.

#include "runtime/engine/context_cache/materialization_budget.h"

#include <cstdio>
#include <vector>

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

using ninfer::runtime::PublicationCellProbe;
using ninfer::runtime::publication_cell_loss;

PublicationCellProbe probe(std::uint32_t cell_only, std::uint32_t other, std::uint32_t goals) {
    PublicationCellProbe value;
    value.cell_only = cell_only;
    value.other     = other;
    value.goals     = goals;
    value.probes    = cell_only + other + goals;
    return value;
}

// A candidate whose every failed probe failed on the cell, and which was never adopted.
constexpr PublicationCellProbe kBlockedByCellOnly = {.probes = 3, .cell_only = 3, .other = 0, .goals = 0};

}  // namespace

int main() {
    // 1. THE WINNER IS EXCLUDED. Its own probes fail on the cell on the way to taking an eviction, which is
    //    the ordinary path; counting that is the defect this test exists for.
    {
        std::vector<PublicationCellProbe> probes = {kBlockedByCellOnly};
        const auto loss = publication_cell_loss(probes, 1, 0, 0, [](std::size_t) { return 99U; });
        check(loss.candidates == 0, "the winner's own cell-only probes are not a loss");
    }

    // 2. A BLOCKED CANDIDATE THAT WOULD HAVE REUSED MORE IS ONE LOSS.
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{}, kBlockedByCellOnly};
        const auto loss = publication_cell_loss(probes, 2, 0, 100, [](std::size_t i) {
            return i == 1U ? 45815U : 100U;
        });
        check(loss.candidates == 1, "a cell-blocked candidate with more reuse counts once");
        check(loss.best_blocked_reuse == 45815U, "and its reuse is the reported best_blocked_reuse");
    }

    // 3. REUSING NO MORE THAN THE WINNER IS NOT A LOSS -- the cell cost this request nothing.
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{}, kBlockedByCellOnly};
        const auto loss = publication_cell_loss(probes, 2, 0, 100, [](std::size_t i) {
            return i == 1U ? 100U : 100U;
        });
        check(loss.candidates == 0, "reusing exactly the winner's amount is not a loss");
    }

    // 4. A FAILURE FOR ANY OTHER REASON DISQUALIFIES THE CANDIDATE: the cell was not the only blocker, so
    //    this is not attributable to the catalog.
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{}, probe(3, 1, 0)};
        const auto loss = publication_cell_loss(probes, 2, 0, 0, [](std::size_t) { return 99999U; });
        check(loss.candidates == 0, "a candidate that also failed on something else is not a cell loss");
    }

    // 5. A CANDIDATE THAT PRODUCED A GOAL COULD BE ADOPTED, so the cell did not block it.
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{}, probe(2, 0, 1)};
        const auto loss = publication_cell_loss(probes, 2, 0, 0, [](std::size_t) { return 99999U; });
        check(loss.candidates == 0, "a candidate with a successful goal is not blocked");
    }

    // 6. NO CELL-ONLY FAILURE IS NOT A LOSS. Without this, any unadoptable candidate would count.
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{}, probe(0, 4, 0)};
        const auto loss = publication_cell_loss(probes, 2, 0, 0, [](std::size_t) { return 99999U; });
        check(loss.candidates == 0, "a candidate with no cell-only failure is not a loss");
    }

    // 6b. AN UNCLASSIFIED PROBE IS NOT A LOSS EITHER -- probes counted with no reason recorded. The struct's
    //     fields are independent, so this shape is reachable by any future caller that counts without
    //     classifying, and it must not be read as "the cell blocked it". (Distinct from case 6, which has a
    //     non-cell reason recorded; this is the case that makes the `cell_only == 0` guard testable on its
    //     own.)
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{},
                                                   PublicationCellProbe{.probes = 3, .cell_only = 0,
                                                                        .other = 0, .goals = 0}};
        const auto loss = publication_cell_loss(probes, 2, 0, 0, [](std::size_t) { return 99999U; });
        check(loss.candidates == 0, "an unclassified probe is not a cell loss");
    }

    // 7. NEVER PROBED IS NOT A LOSS -- an untouched candidate is not evidence about the cell. The two reuse
    //    values differ ON PURPOSE: the never-probed candidate (index 1) reuses MORE than the blocked one
    //    (index 2), so if it were counted the best would be 99999 and the count 2. (Its first version put
    //    the blocked candidate at the winner's index, so it asserted 0 through the winner exclusion and
    //    tested nothing it named.)
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{}, PublicationCellProbe{},
                                                    kBlockedByCellOnly};
        const auto loss = publication_cell_loss(probes, 3, 0, 0, [](std::size_t i) {
            return i == 1U ? 99999U : 50000U;
        });
        check(loss.candidates == 1, "only the PROBED blocked candidate counts, not the untouched one");
        check(loss.best_blocked_reuse == 50000U, "and the untouched candidate's reuse is not the best");
    }

    // 8. TWO BLOCKED CANDIDATES COUNT TWICE, and the best reuse is the maximum, not the last or the first.
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{}, kBlockedByCellOnly, kBlockedByCellOnly};
        const auto loss = publication_cell_loss(probes, 3, 0, 10, [](std::size_t i) {
            return i == 1U ? 200U : 900U;
        });
        check(loss.candidates == 2, "two blocked candidates count twice");
        check(loss.best_blocked_reuse == 900U, "best_blocked_reuse is the maximum over them");
    }

    // 9. BOUNDS: a tally shorter than the candidate list must not be read past its end. The planner can add
    //    candidates while the tally grows lazily, so this is a live shape, not a hypothetical.
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{}};
        const auto loss = publication_cell_loss(probes, 40, 0, 0, [](std::size_t) { return 99999U; });
        check(loss.candidates == 0, "a candidate list longer than the tally is not read out of bounds");
    }

    // 10. THE AT-RISK BREAKDOWN, which exists to settle whether a loss is REACHABLE (see the struct's
    //     comment): every non-winner candidate that hit the cell counts as at-risk, and the veto fields say
    //     which single condition disqualified each. Without this, `at_risk` could be wrong in one direction
    //     and the reachability question would be answered by a broken meter.
    {
        std::vector<PublicationCellProbe> probes = {
            PublicationCellProbe{},   // 0: the winner, never probed
            probe(2, 0, 1),           // 1: at risk, but a goal existed
            probe(2, 1, 0),           // 2: at risk, also failed on something else
            kBlockedByCellOnly,       // 3: at risk, would not out-reuse the winner
            kBlockedByCellOnly,       // 4: at risk AND would out-reuse -> the one loss
        };
        const auto loss = publication_cell_loss(probes, 5, 0, 100, [](std::size_t i) {
            return i == 3U ? 100U : 50000U;
        });
        check(loss.at_risk == 4, "every non-winner candidate with a cell-only failure is at risk");
        check(loss.blocked_by_goals == 1, "one at-risk candidate is vetoed by having produced a goal");
        check(loss.blocked_by_other == 1, "one is vetoed by another failure");
        check(loss.blocked_by_reuse == 1, "one is vetoed by not out-reusing the winner");
        check(loss.candidates == 1, "and exactly one counts as a loss");
    }

    // 11. THE AT-RISK METER EXCLUDES THE WINNER TOO, and that is a DIFFERENT exclusion from the loss's: the
    //     winner's own cell-only probes are expected, so they must not inflate `at_risk` either. A mutant that
    //     counts the winner in `at_risk` while still excluding it from `candidates` passed every earlier case,
    //     because their winners had no cell-only probe -- so the winner here HAS one and must not appear.
    {
        std::vector<PublicationCellProbe> probes = {kBlockedByCellOnly, PublicationCellProbe{}};
        const auto loss = publication_cell_loss(probes, 2, 0, 0, [](std::size_t) { return 7U; });
        check(loss.at_risk == 0, "the winner's cell-only probes are not at risk either");
        check(loss.candidates == 0, "and it is of course not a loss");
    }

    // 12. WHEN A CANDIDATE CARRIES BOTH VETOES, THE GOAL WINS, and that ordering is a decision rather than an
    //     accident: a candidate with a successful goal was adoptable, so the cell is not what stopped it --
    //     reporting the other veto instead would name the wrong cause. A mutant swapping the two checks passed
    //     every earlier case, because none of them had a candidate with both.
    {
        std::vector<PublicationCellProbe> probes = {PublicationCellProbe{}, probe(2, 3, 1)};
        const auto loss = publication_cell_loss(probes, 2, 0, 0, [](std::size_t) { return 99999U; });
        check(loss.at_risk == 1, "a candidate with both a goal and another failure is at risk once");
        check(loss.blocked_by_goals == 1, "and it is reported under the goal veto, which is the deciding one");
        check(loss.blocked_by_other == 0, "not under the other veto it also carries");
        check(loss.candidates == 0, "and it is not a loss");
    }

    if (failures != 0) {
        std::printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("all publication-cell-loss checks passed\n");
    return 0;
}
