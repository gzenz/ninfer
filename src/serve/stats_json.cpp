#include "serve/stats_json.h"

#include <nlohmann/json.hpp>

namespace ninfer::serve {

// The `demote_refusal_at_eviction_*` keys are now DERIVED from `ninfer::k_demote_refusal_names` (see the
// loop below the `pressure` block), so an added clause needs no edit here and a renamed or reordered one
// moves its key with it. The count assert that used to sit here is therefore GONE rather than kept: its job
// was to force a hand-written list to be extended, and with derivation that list no longer exists -- keeping
// it would fire on a legitimate addition, and a check that fails on correct code is a check people learn to
// bypass. The name-to-key mapping has ONE definition, and `sentinel`-style reachability for it is the
// all-ten literal pin in `ninfer_resource_manager_test`.

std::string format_stats_json(const ninfer::RuntimeStats& s, const ninfer::MemorySummary& m,
                              const ninfer::LoadSummary& l,
                              const ninfer::ContextCacheOptions& cache,
                              std::size_t http_in_flight, std::size_t http_max_in_flight) {
    using nlohmann::json;
    json j;
    j["timestamp_unix_ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    j["http"] = {
        {"in_flight", http_in_flight},
        {"max_in_flight", http_max_in_flight},
    };

    j["scheduler"] = {
        {"running", s.running_requests},
        {"prefilling", s.prefilling_requests},
        {"decode_ready", s.decode_ready_requests},
        {"waiting", s.waiting_requests},
        {"materializing", s.materializing_requests},
        {"capture_pending", s.capture_pending_requests},
        {"terminal_pending", s.terminal_pending_requests},
    };

    j["counters"] = {
        {"computed_prefill_tokens", s.computed_prefill_tokens},
        {"committed_decode_tokens", s.committed_decode_tokens},
        {"decode_rounds", s.decode_rounds},
        {"decode_row_rounds", s.decode_row_rounds},
        {"active_captures_completed", s.active_captures_completed},
        {"active_captures_aborted", s.active_captures_aborted},
    };

    j["cache_reuse"] = {
        {"root_selections", s.root_selections},
        {"private_endpoint_selections", s.private_endpoint_selections},
        {"private_turn_closure_selections", s.private_turn_closure_selections},
        {"private_response_replay_selections", s.private_response_replay_selections},
        {"private_long_anchor_selections", s.private_long_anchor_selections},
        {"shared_stable_prefix_selections", s.shared_stable_prefix_selections},
        {"reused_prompt_tokens", s.reused_prompt_tokens},
        {"last_selected_frontier_tokens", s.last_selected_frontier_tokens},
        {"historical_fork_hits", s.historical_fork_hits},
    };

    // FLAT keys — the monitor (and our v2 /stats) read these flat, not nested.
    j["kv_transfers"] = {
        {"main_kv_d2h_pages", s.main_kv_d2h_pages},
        {"main_kv_h2d_pages", s.main_kv_h2d_pages},
        {"main_kv_d2d_pages", s.main_kv_d2d_pages},
        {"main_kv_d2h_bytes", s.main_kv_d2h_bytes},
        {"main_kv_h2d_bytes", s.main_kv_h2d_bytes},
        {"main_kv_d2d_bytes", s.main_kv_d2d_bytes},
        {"main_kv_d2h_seconds", s.main_kv_d2h_seconds},
        {"main_kv_h2d_seconds", s.main_kv_h2d_seconds},
        {"main_kv_d2d_seconds", s.main_kv_d2d_seconds},
        {"backend_kv_d2h_pages", s.backend_kv_d2h_pages},
        {"backend_kv_h2d_pages", s.backend_kv_h2d_pages},
        {"backend_kv_d2d_pages", s.backend_kv_d2d_pages},
        {"backend_kv_d2h_bytes", s.backend_kv_d2h_bytes},
        {"backend_kv_h2d_bytes", s.backend_kv_h2d_bytes},
        {"backend_kv_d2d_bytes", s.backend_kv_d2d_bytes},
        {"backend_kv_d2h_seconds", s.backend_kv_d2h_seconds},
        {"backend_kv_h2d_seconds", s.backend_kv_h2d_seconds},
        {"backend_kv_d2d_seconds", s.backend_kv_d2d_seconds},
    };

    // FLAT keys (v2 shape) + upstream extras (moves/forks/restores/d2d_seconds).
    j["state_transfers"] = {
        {"state_d2h_count", s.state_d2h_count},
        {"state_h2d_count", s.state_h2d_count},
        {"state_d2d_count", s.state_d2d_count},
        {"state_d2h_bytes", s.state_d2h_bytes},
        {"state_h2d_bytes", s.state_h2d_bytes},
        {"state_d2d_bytes", s.state_d2d_bytes},
        {"state_d2h_seconds", s.state_d2h_seconds},
        {"state_h2d_seconds", s.state_h2d_seconds},
        {"state_d2d_seconds", s.state_d2d_seconds},
        {"state_moves", s.state_moves},
        {"state_forks", s.state_forks},
        {"state_restores", s.state_restores},
    };

    j["pressure"] = {
        {"device_main_kv_occupied_pages", s.device_main_kv_occupied_pages},
        // The ceiling in the SAME units, so the pair reads as a fraction.
        {"device_main_kv_capacity_pages", s.device_main_kv_capacity_pages},
        {"device_backend_kv_capacity_pages", s.device_backend_kv_capacity_pages},
        {"device_backend_kv_occupied_pages", s.device_backend_kv_occupied_pages},
        {"device_state_occupied_slots", s.device_state_occupied_slots},
        // The ceiling, so a reader can tell a FULL device state pool from a barely-used one.
        {"device_state_capacity_slots", s.device_state_capacity_slots},
        {"host_kv_occupied_bytes", s.host_kv_occupied_bytes},
        {"host_state_occupied_slots", s.host_state_occupied_slots},
        {"spill_pages", s.pressure_spill_pages},
        {"partial_tail_cow_pages", s.partial_tail_cow_pages},
        {"private_owners_degraded", s.pressure_private_owners_degraded},
        {"private_owners_demoted", s.pressure_private_owners_demoted},
        {"private_owners_demoted_kv", s.pressure_private_owners_demoted_kv},
        {"private_owners_demoted_kv_only", s.pressure_private_owners_demoted_kv_only},
        {"private_owners_evicted", s.pressure_private_owners_evicted},
        {"shared_owners_degraded", s.pressure_shared_owners_degraded},
        {"shared_owners_evicted", s.pressure_shared_owners_evicted},
        {"shared_owners_replaced", s.pressure_shared_owners_replaced},
        // THE TWO HALVES, SUMMED. `private_owners_evicted` and `shared_owners_evicted` are BOTH private-or-
        // shared-scoped, and the `private victim evicted:` journal line is PRIVATE-ONLY (its loop runs over
        // `transaction.victim_count`), so this is the number to compare against a whole-engine total.
        // **DO NOT compare it against the request log's `chosen_restorable_evictions` and expect agreement
        // -- they are DIFFERENT POPULATIONS.** This counts owners COMMITTED as `Evicted`, restorable or not.
        // That one counts the chosen PLAN's predicted evictions that held a recoverable checkpoint. The
        // relation is plan-vs-committed, not two scopes of one count, and the disconnect is documented in
        // the repo (the plan count UNDERCOUNTS the committed set).
        // **An earlier comment here claimed the halves "add up to what the plan counted", on 13 + 5 = 18 in
        // one request. A review found a second coincidence (60 and 60) and killed the inference: two
        // coincidences are not a mechanism.** This field is DERIVED rather than stored, because a third
        // counter that can drift from its halves is worse than an addition done where they are serialized.
        {"owners_evicted_total", s.pressure_private_owners_evicted + s.pressure_shared_owners_evicted},
        // The halves keep their scope-naming and are NOT renamed: `demotable` is the monitor's alert token
        // and moving what it counts would silently change the meaning of every past reading.
        {"private_evictions_demotable", s.pressure_private_evictions_demotable},
        {"evictions_with_victim_room", s.pressure_evictions_with_victim_room},
        // WHICH AXIS EACH EVICTION RELIEVED. Counted over the whole population because the eviction PRINT
        // is rate-limited (first 8, then every 512th), so the axis of the population could not be read off
        // it. A victim holding both axes is counted in both -- these do not sum to the eviction count.
        {"evictions_victim_device_slots", s.pressure_evictions_victim_device_slots},
        {"evictions_victim_host_slots", s.pressure_evictions_victim_host_slots},
        // THE NEEDED AXIS beside the RELIEVED one: the plan's deficit at each eviction. See types.h for why
        // the pair is required and why they do not sum to `private_eviction_checks`.
        {"evictions_deficit_device_main_kv", s.pressure_evictions_deficit_device_main_kv},
        {"evictions_deficit_device_backend_kv", s.pressure_evictions_deficit_device_backend_kv},
        {"evictions_deficit_device_state", s.pressure_evictions_deficit_device_state},
        {"evictions_deficit_host_state", s.pressure_evictions_deficit_host_state},
        {"evictions_deficit_host_kv", s.pressure_evictions_deficit_host_kv},
        {"evictions_kv_pages", s.pressure_evictions_kv_pages},
        {"evictions_kv_pages_total", s.pressure_evictions_kv_pages_total},
        {"evictions_kv_fit_pinned", s.pressure_evictions_kv_fit_pinned},
        {"evictions_kv_fit_growable", s.pressure_evictions_kv_fit_growable},
        {"evictions_kv_fit_blocked", s.pressure_evictions_kv_fit_blocked},
        {"evictions_kv_fit_pinned_pages", s.pressure_evictions_kv_fit_pinned_pages},
        {"evictions_kv_fit_growable_pages", s.pressure_evictions_kv_fit_growable_pages},
        {"evictions_kv_fit_blocked_pages", s.pressure_evictions_kv_fit_blocked_pages},
        // THE PER-AXIS SHORTFALLS, each with its denominator first so a zero is readable. The device-KV
        // pair is the one that carries most of the eviction deficit; `max_pages` is the magnitude that
        // separates "the peak misses by a little" from "the peak misses by a factor".
        {"physical_peak_fits_calls", s.pressure_physical_peak_fits_calls},
        {"host_state_fit_checks", s.pressure_host_state_fit_checks},
        {"host_state_shortfall_calls", s.pressure_host_state_shortfall_calls},
        {"host_state_shortfall_max_slots", s.pressure_host_state_shortfall_max_slots},
        {"device_main_kv_fit_checks", s.pressure_device_main_kv_fit_checks},
        {"device_main_kv_shortfall_calls", s.pressure_device_main_kv_shortfall_calls},
        {"device_main_kv_shortfall_max_pages", s.pressure_device_main_kv_shortfall_max_pages},
        // The conjunction: victim_room AND demote_possible. This IS the #6 count.
        {"evictions_demote_possible", s.pressure_evictions_demote_possible},
        // THE REFUSAL REASON AT EACH PRIVATE, MATERIALIZATION-PATH EVICTION -- not "every eviction", which
        // this comment claimed until a review measured it: `capture.cpp` releases and shared evictions do not
        // reach this counter, so the sum invariant is a consistency check on ONE path.
        // One key per clause IN `k_demote_refusal_names` ORDER. The journal's `demote_refusal=` field is
        // rate-limited to the first 8 then every 512th, so it can only ever sample the START of a window
        // (measured 7-of-8 `already-on-host`, which is NOT benign -- state on host + evicted destroys the
        // copy); these buckets are the whole population. Denominator: `private_eviction_checks`.
        // NOTE: `evictions_demote_possible` is NOT attributable here and cannot be -- `demote_possible` IS
        // `demote_refusal == None`, so those evictions are `none` by construction. These buckets describe the
        // OTHER population: the evictions where a demote was NOT available.
        // THE TEN KEYS ARE NOT LISTED HERE. They are emitted by the DERIVED loop below the `pressure` block,
        // because as literal strings they were a second copy of the names: `k_demote_refusal_names[3]` and
        // `[4]` swapped would have passed every assert and test while silently splitting the journal's
        // `demote_refusal=` token from the key it is supposed to agree with. A review caught that on
        // 2026-10-04. Deriving them makes the table the only source, which is what the comments always said.
        {"private_eviction_checks", s.pressure_private_eviction_checks},
        {"options", s.pressure_options},
        {"demote_options", s.pressure_demote_options},
        // The three ways a demote option is NOT generated. `evicting_current` is the policy choice.
        {"options_refused_no_kv", s.pressure_options_refused_no_kv},
        // `no_state_deficit` = wrong relief axis (the fix); `precondition` = no demote existed (correct).
        {"demote_option_refused_no_state_deficit", s.pressure_demote_option_refused_no_state_deficit},
        // The preservation probe. **`successors_without_option` counts the owners that had NO non-evicting
        // successor; the key was named `successors_with_option` until 2026-10-06 and said the OPPOSITE.**
        // Owners WITH a preserving alternative = `successor_calls - successors_without_option`.
        {"successor_calls", s.pressure_successor_calls},
        {"successors_without_option", s.pressure_successors_without_option},
        // Calls whose set was empty BY CONSTRUCTION (the current decision evicts); 0 on the #6 repro.
        {"successor_calls_evicting_current", s.pressure_successor_calls_evicting_current},
        {"demote_option_refused_precondition", s.pressure_demote_option_refused_precondition},
        {"options_refused_active_lanes", s.pressure_options_refused_active_lanes},
        {"options_refused_evicting_current", s.pressure_options_refused_evicting_current},
        {"capture_skips_transaction_or_fork", s.capture_skips_transaction_or_fork},
        {"capture_skips_cancelled", s.capture_skips_cancelled},
        {"capture_skips_nothing_to_publish", s.capture_skips_nothing_to_publish},
        {"capture_skips_stale_pressure_plan", s.capture_skips_stale_pressure_plan},
        {"capture_skips_not_feasible_no_pressure", s.capture_skips_not_feasible_no_pressure},
        {"publication_cell_losses", s.pressure_publication_cell_losses},
        // The denominator beside it: goal-probe calls, not an outcome. See the RuntimeStats comment.
        {"publication_cell_probes", s.pressure_publication_cell_probes},
        // Shared-prefix release refusals, per clause. Four, not one: a stale handle is a caller bug while an
        // in-flight transaction is contention, and a single total would answer neither. Always on -- the
        // `[release-probe]` print these replace was gated behind an env var set in no recorded run.
        // THE DENOMINATOR BESIDE THEM, and it is not decoration: `refusals == 0` with `attempts == 0` is an
        // UNMEASURED release path, not a clean one. Same reading rule as `publication_cell_probes`.
        {"shared_prefix_release_attempts", s.shared_prefix_release_attempts},
        {"shared_prefix_release_refusals_stale_index", s.shared_prefix_release_refusals_stale_index},
        {"shared_prefix_release_refusals_in_flight", s.shared_prefix_release_refusals_in_flight},
        {"shared_prefix_release_refusals_not_releasable", s.shared_prefix_release_refusals_not_releasable},
        {"shared_prefix_release_refusals_check_threw", s.shared_prefix_release_refusals_check_threw},
        // The demand window's bits by evidence. `considered` counts OFFERS, not uses.
        {"demand_bits_selected", s.pressure_demand_bits_selected},
        {"demand_bits_resident", s.pressure_demand_bits_resident},
        {"demand_bits_considered", s.pressure_demand_bits_considered},
        // Uncapped, because the journal line that reports these is rate-limited to 8 then every 512th.
        {"publication_cell_at_risk_runs", s.pressure_publication_cell_at_risk_runs},
        // WHY GOALS FAILED, UNCONDITIONALLY -- see the RuntimeStats comment. The `veto_*` fields below are
        // gated on `at_risk != 0` and read 0 on a run where nothing out-reuses the winner, while thousands of
        // goals can still be refused; these two are the ungated version of the same split.
        {"goal_blocked_cell_only", s.pressure_goal_blocked_cell_only},
        {"goal_blocked_other", s.pressure_goal_blocked_other},
        {"publication_cell_veto_goals", s.pressure_publication_cell_veto_goals},
        {"publication_cell_veto_other", s.pressure_publication_cell_veto_other},
        {"publication_cell_veto_reuse", s.pressure_publication_cell_veto_reuse},
        {"checkpoints_dropped", s.pressure_checkpoints_dropped},
        {"searches", s.pressure_searches},
        {"search_budget_exhaustions", s.pressure_search_budget_exhaustions},
        {"target_arena_truncations", s.pressure_target_arena_truncations},
        {"maximal_fallback_selections", s.pressure_maximal_fallback_selections},
        {"shared_active_references", s.shared_active_references},
        {"actual_context_transfer_seconds", s.actual_context_transfer_seconds},
    };

    // THE TEN DEMOTE-REFUSAL BUCKETS, DERIVED from the one name table rather than hand-typed: the key is
    // `demote_refusal_at_eviction_` + the clause name with `-` -> `_`. This lives here rather than inside the
    // `j["pressure"] = {...}` initializer above because it is a loop, and a hand-typed list is exactly the
    // drift this derivation removes.
    for (std::size_t bucket = 0; bucket < ninfer::k_demote_refusal_buckets; ++bucket) {
        std::string key = "demote_refusal_at_eviction_";
        for (const char* c = ninfer::k_demote_refusal_names[bucket]; c != nullptr && *c != '\0'; ++c) {
            key.push_back(*c == '-' ? '_' : *c);
        }
        j["pressure"][key] = s.pressure_demote_refusal_at_eviction[bucket];
    }

    // WHY an empty successor set was empty, DERIVED from `k_successor_empty_reason_names` for the same
    // reason the buckets above are. Read `no_endpoints` AGAINST `all_refused`: the first says the residual
    // this call was given asked for NOTHING, so there was nothing to relieve; the second says it asked and
    // the option builder refused. One number for the two is what made the #6 rate unreadable.
    for (std::size_t reason = 0; reason < ninfer::k_successor_empty_reason_count; ++reason) {
        std::string key = "successor_empty_";
        for (const char* c = ninfer::k_successor_empty_reason_names[reason]; c != nullptr && *c != '\0'; ++c) {
            key.push_back(*c == '-' ? '_' : *c);
        }
        j["pressure"][key] = s.pressure_successor_empty_by_reason[reason];
    }

    // WHY A DEVICE-KV PAGE WAS REJECTED AS A RELIEF CANDIDATE, per clause and per direction, DERIVED from the
    // two name tables. **Read the `demote` row against its own total**: `active_reference` dominating it is
    // the mechanism #6's last question names -- a demote cannot touch an active victim's KV, so a preserving
    // plan cannot relieve the axis it is short on -- while any other clause dominating it would refute that.
    for (std::size_t direction = 0; direction < ninfer::k_kv_relief_direction_count; ++direction) {
        for (std::size_t clause = 0; clause < ninfer::k_kv_relief_clause_count; ++clause) {
            std::string key = std::string("kv_relief_rejected_") +
                              ninfer::k_kv_relief_direction_names[direction] + "_";
            for (const char* c = ninfer::k_kv_relief_clause_names[clause];
                 c != nullptr && *c != '\0'; ++c) {
                key.push_back(*c == '-' ? '_' : *c);
            }
            j["pressure"][key] = s.pressure_kv_relief_rejected[direction][clause];
        }
        // WHAT REACHED THE PEAK, which the rejection histogram cannot show. Read `relieved` AGAINST
        // `requested`: an ASKED-BUT-NOT-FOUND demote is a different mechanism from a demote that was never
        // asked, and the two passes share one budget with the drop pass running FIRST.
        // `requested` EXCLUDES the "relieve as much as you can" sentinel, which is counted on its own -- the
        // first version of this key summed the sentinel and produced 8.5e12 "pages", a number that measured
        // nothing. Read `calls` and `saturated_asks` beside it or the sum is still not a denominator.
        j["pressure"][std::string("kv_relief_calls_") +
                      ninfer::k_kv_relief_direction_names[direction]] =
            s.pressure_kv_relief_calls[direction];
        j["pressure"][std::string("kv_relief_saturated_asks_") +
                      ninfer::k_kv_relief_direction_names[direction]] =
            s.pressure_kv_relief_saturated_asks[direction];
        j["pressure"][std::string("kv_relief_max_ask_") +
                      ninfer::k_kv_relief_direction_names[direction]] =
            s.pressure_kv_relief_max_ask[direction];
        j["pressure"][std::string("kv_relief_requested_") +
                      ninfer::k_kv_relief_direction_names[direction]] =
            s.pressure_kv_relief_requested[direction];
        j["pressure"][std::string("kv_relief_relieved_") +
                      ninfer::k_kv_relief_direction_names[direction]] =
            s.pressure_kv_relief_relieved[direction];
    }
    // A DEMOTE THAT NEVER RAN IS NOT A DEMOTE THAT FOUND NOTHING.
    for (std::size_t clause = 0; clause < ninfer::k_kv_relief_gate_clause_count; ++clause) {
        j["pressure"][std::string("kv_relief_demote_gate_blocked_") +
                      ninfer::k_kv_relief_gate_clause_names[clause]] =
            s.pressure_kv_relief_gate_blocks[clause];
    }
    // WHICH AXIS KILLED A TARGET, split by whether the target evicts. The global per-axis array sums over
    // every call site; this one is taken AT THE FEASIBILITY SITE, which is where a preserving plan lives or
    // dies. `preserving` = the target's own decisions carry no eviction (the signal available there).
    for (std::size_t preserving = 0; preserving < 2; ++preserving) {
        for (std::size_t axis = 0; axis < ninfer::k_peak_axis_count; ++axis) {
            std::string key = std::string("peak_refusals_") +
                              (preserving == 0 ? "preserving_" : "evicting_") +
                              ninfer::k_peak_axis_names[axis];
            j["pressure"][key] = s.pressure_peak_refusals_by_preserving[preserving][axis];
        }
    }
    // THE PEAK ARITHMETIC, per axis. `added` against `removed` is the model: an axis that refuses because
    // `added` dwarfs `removed` is a different defect from one where both are large and only the SUM misses.
    // THE QUANTITY THAT SEPARATES #6's TWO CANDIDATE REMEDIES. Read `max`/`mean` against the reservation
    // (`device_main_kv_shortfall_max_pages`) and the observed per-step relief (`peak_model_removed_*`).
    // WHICH TERM OF THE RESERVATION DOMINATES. The peak a plan is judged on IS this reservation, so a
    // remedy aimed at "the reservation" without this split is aimed at a sum.
    // THE ENTITLEMENT'S OWN SPLIT: is the reservation the prompt, the output budget, or both?
    // THE PLAN-LEVEL RELIEF OF EACH OPTION, same victim, same moment. Read `evicting` AGAINST `preserving`
    // per axis: the VICTIM-level figures are equal, so a gap here is where the two plans differ.
    // THE PER-INSTANCE EVIDENCE, as an ARRAY: the means could not decide #6, because the MEAN reservation
    // makes an eviction's own arithmetic fail too while evictions are admitted. `_total` is the denominator and
    // says when the ring wrapped; entries are chronological, oldest first.
    // THE VICTIM'S OWN FIGURES AT THE DECISION SITE -- the other half of the 13% decision gap, in the SAME run.
    j["pressure"]["victim_ownership_calls"]        = s.pressure_victim_ownership_calls;
    j["pressure"]["victim_ownership_release_sum"]  = s.pressure_victim_ownership_release_sum;
    j["pressure"]["victim_ownership_resident_sum"] = s.pressure_victim_ownership_resident_sum;
    j["pressure"]["victim_ownership_gap_sum"]      = s.pressure_victim_ownership_gap_sum;
    j["pressure"]["victim_ownership_gap_max"]      = s.pressure_victim_ownership_gap_max;
    j["pressure"]["assessed_peak_records_total"] = s.pressure_assessed_peak_records_total;
    // THE AGGREGATE: `total - feasible - host_blocked` is what the peak AXES must account for.
    // WHAT AN ADMITTED PLAN HAS THAT A REFUSED ONE DOES NOT: a bigger relief, or a smaller reservation.
    for (std::size_t bucket = 0; bucket < 4; ++bucket) {
        const char* prefix = bucket >= 2 ? "assessed_feasible_" : "assessed_refused_";
        const char* who    = (bucket % 2) == 1 ? "preserving_" : "evicting_";
        const std::string base = std::string(prefix) + who;
        j["pressure"][base + "count"]       = s.pressure_assessed_agg_count[bucket];
        j["pressure"][base + "reservation"] = s.pressure_assessed_agg_reservation[bucket];
        j["pressure"][base + "removed"]     = s.pressure_assessed_agg_removed[bucket];
        j["pressure"][base + "selected"]    = s.pressure_assessed_agg_selected[bucket];
        j["pressure"][base + "evicting"]    = s.pressure_assessed_agg_evicting[bucket];
        j["pressure"][base + "best_available"] = s.pressure_assessed_agg_available[bucket];
        j["pressure"][base + "best_chosen"]    = s.pressure_assessed_agg_chosen[bucket];
        j["pressure"][base + "larger_available"] = s.pressure_assessed_larger_available[bucket];
        j["pressure"][base + "chose_largest"]    = s.pressure_assessed_chose_largest[bucket];
        j["pressure"][base + "kv_change_entries"] = s.pressure_assessed_agg_kv_entries[bucket];
        // *** THE PER-AXIS SHORTFALL OVER ALL ASSESSMENTS -- THE FIGURE A MAGNITUDE IS QUOTED FROM. ***
        // The per-row `axis_shortfall` in `assessed_peak_records` is a 64-row WINDOW (arm A35: 5,912
        // assessments), so its shape may be read per row and its MAGNITUDE must not be read at all. This is
        // the sum over every assessment in the bucket; divide it by the same bucket's `count` for the mean.
        // The axis names are derived at runtime, so THE DEPLOY CHECK'S STRING MUST BE `_axis_shortfall_`.
        for (std::size_t axis = 0; axis < ninfer::k_peak_axis_count; ++axis) {
            j["pressure"][base + "axis_shortfall_" + ninfer::k_peak_axis_names[axis]] =
                s.pressure_assessed_axis_shortfall[bucket][axis];
        }
    }
    for (std::size_t klass = 0; klass < 2; ++klass) {
        const char* who = klass == 0 ? "evicting_" : "preserving_";
        j["pressure"][std::string("assessed_total_") + who]                = s.pressure_assessed_total[klass];
        j["pressure"][std::string("assessed_feasible_") + who]             = s.pressure_assessed_feasible[klass];
        j["pressure"][std::string("assessed_refused_host_blocked_") + who] = s.pressure_assessed_refused_host_blocked[klass];
    }
    {
        nlohmann::json rows = nlohmann::json::array();
        const std::size_t shown = static_cast<std::size_t>(std::min<std::uint64_t>(
            s.pressure_assessed_peak_records_total, ninfer::k_assessed_peak_record_capacity));
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& rec = s.pressure_assessed_peak_records[index];
            // KEYED BY AXIS NAME, NOT BY ORDINAL. A reader who sees `device_backend_kv_pages: 0` can trust
            // the label; an array of six numbers indexed by position is exactly the shape that turns a wrong
            // order into a finding. The names are `k_peak_axis_names`, so the DEPLOY CHECK'S STRING MUST BE
            // THIS LITERAL (`axis_shortfall`) AND NOT A KEY -- the keys are built at runtime and cannot be
            // grepped out of the binary.
            nlohmann::json axis_shortfall = nlohmann::json::object();
            for (std::size_t axis = 0; axis < ninfer::k_peak_axis_count; ++axis) {
                axis_shortfall[ninfer::k_peak_axis_names[axis]] = rec.axis_shortfall[axis];
            }
            rows.push_back({{"peak_main_kv", rec.peak_main_kv},
                            {"peak_backend_kv", rec.peak_backend_kv},
                            {"occupied_main_kv", rec.occupied_main_kv},
                            {"occupied_backend_kv", rec.occupied_backend_kv},
                            {"capacity_main_kv", rec.capacity_main_kv},
                            {"preserving", rec.preserving},
                            {"feasible", rec.feasible},
                            {"refusal_mask", rec.refusal_mask},
                            {"reservation_main_kv", rec.reservation_main_kv},
                            {"removed_main_kv", rec.removed_main_kv},
                            {"evicted_victim_main_kv", rec.evicted_victim_main_kv},
                            {"axis_shortfall", std::move(axis_shortfall)}});
        }
        j["pressure"]["assessed_peak_records"] = std::move(rows);
    }
    // The two counts share a denominator (one note per victim), so `evict/call` and `pres/call` compare.
    j["pressure"]["successor_relief_preserving_options_seen"] =
        s.pressure_successor_relief_preserving_options_seen;
    for (std::size_t slot = 0; slot < 2; ++slot) {
        const char* who = slot == 0 ? "evicting_" : "preserving_";
        j["pressure"][std::string("successor_relief_count_") + who]       = s.pressure_successor_relief_count[slot];
        j["pressure"][std::string("successor_relief_main_kv_") + who]     = s.pressure_successor_relief_main_kv[slot];
        j["pressure"][std::string("successor_relief_backend_kv_") + who]  = s.pressure_successor_relief_backend_kv[slot];
        j["pressure"][std::string("successor_relief_state_slots_") + who] = s.pressure_successor_relief_state_slots[slot];
    }
    j["pressure"]["entitlement_split_calls"]           = s.pressure_entitlement_split_calls;
    j["pressure"]["entitlement_split_prompt_tokens"]   = s.pressure_entitlement_split_prompt_tokens;
    j["pressure"]["entitlement_split_output_tokens"]   = s.pressure_entitlement_split_output_tokens;
    j["pressure"]["entitlement_split_reserved_tokens"] = s.pressure_entitlement_split_reserved_tokens;
    j["pressure"]["reservation_composition_calls"] = s.pressure_reservation_composition_calls;
    for (std::size_t term = 0; term < 3; ++term) {
        static constexpr const char* kTermNames[3] = {"base", "shared_replica", "transient_restore"};
        for (std::size_t axis = 0; axis < 2; ++axis) {
            static constexpr const char* kAxisNames[2] = {"main_kv", "backend_kv"};
            j["pressure"][std::string("reservation_") + kTermNames[term] + "_" + kAxisNames[axis]] =
                s.pressure_reservation_composition[term][axis];
        }
    }
    j["pressure"]["victim_entitlement_gap_calls"]     = s.pressure_victim_entitlement_gap_calls;
    j["pressure"]["victim_entitlement_gap_pages_sum"] = s.pressure_victim_entitlement_gap_pages_sum;
    j["pressure"]["victim_entitlement_gap_pages_max"] = s.pressure_victim_entitlement_gap_pages_max;
    j["pressure"]["peak_model_calls"] = s.pressure_peak_model_calls;
    for (std::size_t axis = 0; axis < ninfer::k_peak_axis_count; ++axis) {
        const std::string name = ninfer::k_peak_axis_names[axis];
        j["pressure"]["peak_model_added_" + name]    = s.pressure_peak_model_added[axis];
        j["pressure"]["peak_model_removed_" + name]  = s.pressure_peak_model_removed[axis];
        j["pressure"]["peak_model_peak_max_" + name] = s.pressure_peak_model_peak_max[axis];
    }
    // THE PREDICTION TEST: compare `relieved/call` between the two buckets. If PASS 1 starves PASS 2, the
    // non-zero bucket's demote take is SHORTER -- and the difference is the pages PASS 1 removed from reach.
    j["pressure"]["kv_relief_demote_calls_host_req_zero"]       = s.pressure_kv_relief_demote_calls_host_req_zero;
    j["pressure"]["kv_relief_demote_calls_host_req_nonzero"]    = s.pressure_kv_relief_demote_calls_host_req_nonzero;
    j["pressure"]["kv_relief_demote_relieved_host_req_zero"]    = s.pressure_kv_relief_demote_relieved_host_req_zero;
    j["pressure"]["kv_relief_demote_relieved_host_req_nonzero"] = s.pressure_kv_relief_demote_relieved_host_req_nonzero;
    j["pressure"]["kv_relief_host_blocked_bytes_sum"] = s.pressure_kv_relief_host_blocked_bytes_sum;
    j["pressure"]["kv_relief_host_blocked_bytes_max"] = s.pressure_kv_relief_host_blocked_bytes_max;

    // THE PER-VICTIM EVICTION RECORD, AS AN ARRAY. It is a distribution, so a key per entry would say
    // nothing a reader could read; `eviction_kv_records_total` is its denominator -- when it exceeds the
    // array's length the ring wrapped, and the reader is TOLD that rather than shown a suffix as if it were
    // the window. `fit` is the `host_kv_fit_name` ordinal (0 pinned, 1 growable, 2 blocked); `restorable` is
    // 1 when the victim held a complete state image. Entries are chronological, oldest first.
    j["pressure"]["eviction_kv_records_total"] = s.pressure_eviction_kv_records_total;
    j["pressure"]["eviction_kv_record_count"]  = s.pressure_eviction_kv_record_count;
    j["pressure"]["eviction_kv_records"]       = nlohmann::json::array();
    for (std::uint32_t record = 0; record < s.pressure_eviction_kv_record_count; ++record) {
        const ninfer::RuntimeStats::EvictionKVRecord& entry = s.pressure_eviction_kv_records[record];
        j["pressure"]["eviction_kv_records"].push_back(
            {{"pages", entry.pages},
             {"frontier", entry.frontier},
             {"fit", entry.fit},
             {"restorable", entry.restorable}});
    }

    // WHICH AXIS REFUSED A PLAN, DERIVED from `ninfer::k_peak_axis_names` rather than hand-typed -- the same
    // derivation the demote-refusal buckets use, and for the same reason: a hand-typed list is exactly the
    // drift it would remove. Read the six beside `physical_peak_fits_calls`, which is their denominator; they
    // do NOT sum to the refusals, because one peak can exceed several capacities at once.
    for (std::size_t axis = 0; axis < ninfer::k_peak_axis_count; ++axis) {
        j["pressure"][std::string("physical_peak_refusals_") + ninfer::k_peak_axis_names[axis]] =
            s.pressure_physical_peak_refusals_by_axis[axis];
    }
    // METHOD 2's decision counters are emitted INSIDE the `memory` initializer below, NOT here -- an
    // earlier version of this change set them on `j["memory"]` at this point and the `j["memory"] = {...}`
    // assignment on the next line WIPED them, so the keys were absent from `/stats` while the deploy check
    // passed and the strings were in the binary. **Found by reading the keys rather than trusting the
    // deploy.** A key written before the object it belongs to is replaced does not exist.

    j["memory"] = {
        {"device", m.device},
        {"max_context", m.max_context},
        {"kv_capacity", m.kv_capacity},
        {"kv_capacity_page_groups", m.kv_capacity_page_groups},
        {"kv_capacity_max_page_groups", m.kv_capacity_max_page_groups},
        {"kv_payload_bytes", m.kv_payload_bytes},
        {"kv_capacity_headroom_bytes", m.kv_capacity_headroom_bytes},
        {"runtime_reservation_bytes", m.runtime_reservation_bytes},
        {"available_after_weights_bytes", m.available_after_weights_bytes},
        {"available_after_startup_bytes", m.available_after_startup_bytes},
        {"host_kv_capacity_bytes", m.host_kv_capacity_bytes},
        {"host_kv_occupied_bytes", m.host_kv_occupied_bytes},
        {"host_state_capacity_slots", m.host_state_capacity_slots},
        // THE STATE AXIS'S CEILING (0 = unbounded) and the growth it refused. Read `ceiling_refusals` against
        // `host_state_pregrow_refusals`: the first is room this axis gave to KV ON PURPOSE, the second is the
        // machine being out of room. Only the first is the trade-off being measured.
        {"host_state_max_slots", m.host_state_max_slots},
        {"host_state_growth_ceiling_refusals", m.host_state_growth_ceiling_refusals},
        // From `s`, not `m`: the catalog lives in the ResourceManager (runtime) and the MemorySummary is
        // assembled by ProgramImpl, which cannot see it. Kept in this block anyway so the capacity/occupancy
        // pairs read together.
        {"private_catalog_capacity_cells", s.private_catalog_capacity_cells},
        {"catalog_cell_clears_terminal", s.catalog_cell_clears_terminal},
        // THE HEADROOM-AT-VETO DISCRIMINATOR, beside the blocked counters that are its denominator. Read
        // `host_kv_blocked_within_headroom` against `host_kv_fit_blocked`: a non-zero ratio means `Blocked`
        // verdicts are being returned although the shortfall fitted the headroom AT THAT MOMENT, which the
        // tri-state rule forbids -- so a descriptor guard refused, not the ceiling.
        {"host_kv_blocked_within_headroom", m.host_kv_blocked_within_headroom},
        {"host_kv_blocked_within_headroom_max_bytes", m.host_kv_blocked_within_headroom_max_bytes},        // METHOD 2's decision counters, from `s` for the same reason as the catalog fields above. Read
        // them as a PAIR: a grow for a plan the search does not adopt pins memory for nothing, and a
        // refusal says the admission half alone cannot carry the fix.
        {"host_kv_seal_grows", s.host_kv_seal_grows},
        {"host_kv_seal_refusals", s.host_kv_seal_refusals},
        {"catalog_cell_clears_action", s.catalog_cell_clears_action},
        {"catalog_cell_clears_cancelled", s.catalog_cell_clears_cancelled},
        {"catalog_cell_clears_cleanup", s.catalog_cell_clears_cleanup},
        {"catalog_cell_clears_rollback", s.catalog_cell_clears_rollback},
        {"session_erasures_eviction", s.session_erasures_eviction},
        {"session_erasures_consume", s.session_erasures_consume},
        {"session_index_capacity_cells", s.session_index_capacity_cells},
        {"session_index_occupied_cells", s.session_index_occupied_cells},
        {"host_state_checkpoints_held", s.host_state_checkpoints_held},
        {"host_state_checkpoints_unheld", s.host_state_checkpoints_unheld},
        {"private_catalog_occupied_cells", s.private_catalog_occupied_cells},
        {"host_pinned_capacity_bytes", m.host_pinned_capacity_bytes},
        {"host_pinned_free_bytes", m.host_pinned_free_bytes},
        {"host_pinned_chunks", m.host_pinned_chunks},
        {"host_pinned_grows", m.host_pinned_grows},
        {"host_pinned_grow_refusals", m.host_pinned_grow_refusals},
        // FRAGMENTATION, AND THE REASON A GROWTH WAS REFUSED. The first is the only figure that separates
        // "full" from "fragmented" -- free bytes can be large while nothing large can be placed -- and the
        // pool has computed it since the elastic change without exporting it. The rest turn
        // `host_pinned_grow_refusals` from a total into a diagnosis: policy-vs-pin is where a fix belongs,
        // and `last_veto` with the reading it came from says whether the machine was short, the policy was
        // strict, or /proc/meminfo was unreadable (the fail-closed path, which otherwise hides inside the
        // same number as a full machine).
        {"host_pinned_largest_free_run_bytes", m.host_pinned_largest_free_run_bytes},
        // The headroom the planner's KV feasibility reads. It existed and had a purity test but was never
        // exported, so the `Growable` arm could be exercised and still read 0 here -- indistinguishable from
        // a query that was never wired.
        {"host_pinned_growth_headroom_bytes", m.host_pinned_growth_headroom_bytes},
        {"host_pinned_growth_policy_refusals", m.host_pinned_growth_policy_refusals},
        {"host_pinned_growth_pin_failures", m.host_pinned_growth_pin_failures},
        // Allocation failures. `fragmented_misses` is the fragmentation proof -- the first placement failed
        // while free BYTES were sufficient -- and it is the reachable one; `post_grow_failures` is believed
        // unreachable and is a growth-contract check, NOT evidence. Neither was exported before.
        {"host_pinned_allocation_refusals", m.host_pinned_allocation_refusals},
        // The speculative half, beside the real one -- see the MemorySummary comment.
        {"host_pinned_allocation_speculative_refusals", m.host_pinned_allocation_speculative_refusals},
        {"host_pinned_allocation_ram_refusals", m.host_pinned_allocation_ram_refusals},
        {"host_pinned_allocation_post_grow_failures", m.host_pinned_allocation_post_grow_failures},
        {"host_pinned_allocation_fragmented_misses", m.host_pinned_allocation_fragmented_misses},
        {"host_pinned_reserve_bytes", m.host_pinned_reserve_bytes},
        {"host_pinned_last_veto", m.host_pinned_last_veto},
        {"host_pinned_last_wanted_bytes", m.host_pinned_last_wanted_bytes},
        {"host_pinned_last_veto_mem_available_bytes", m.host_pinned_last_veto_mem_available_bytes},
        {"host_state_pregrow_attempts", m.host_state_pregrow_attempts},
        {"host_state_pregrows", m.host_state_pregrows},
        {"host_state_pregrow_refusals", m.host_state_pregrow_refusals},
        {"host_kv_pregrow_attempts", m.host_kv_pregrow_attempts},
        {"host_kv_pregrows", m.host_kv_pregrows},
        {"host_kv_pregrow_refusals", m.host_kv_pregrow_refusals},
        {"host_kv_pregrow_fragmented", m.host_kv_pregrow_fragmented},
        {"host_kv_fit_growable", m.host_kv_fit_growable},
        {"host_kv_blocked_checks", m.host_kv_blocked_checks},
        // The magnitude beside the count. It is a PROCESS-LIFETIME HIGH-WATER MARK, not a per-request
        // figure -- an earlier version of this line said "request-level", which the owning comment retracts.
        {"host_kv_blocked_max_bytes", m.host_kv_blocked_max_bytes},
        {"host_kv_fit_pinned", m.host_kv_fit_pinned},
        {"host_kv_fit_blocked", m.host_kv_fit_blocked},
        {"host_kv_grows", m.host_kv_grows},
        {"host_kv_grow_refusals", m.host_kv_grow_refusals},
        {"host_state_occupied_slots", m.host_state_occupied_slots},
        {"weights_bytes", m.weights.used_bytes},
        {"sequence_bytes", m.sequence.used_bytes},
        {"workspace_bytes", m.workspace.used_bytes},
        {"workspace_logical_peak_bytes", m.workspace_logical_peak_bytes},
    };

    // Upstream has no host-KV net: emit the flat net counters the dashboard reads
    // (superseded/evictions/compactions/single_alloc_failures/net_state_bytes) as
    // zero, plus the tier census (all-zero) and empty top_units — so a net-less
    // build renders an honest "safety net: 0 entries" panel instead of missing keys.
    //
    // The zeros are placeholders, not measurements, and in a block named `tier_census` they read as
    // "host KV is empty". It is not: `host_kv_occupied_bytes` and `pressure.spill_pages` are the real
    // numbers, and a live run showed 1.06 GB occupied against an all-zero census. So the block now
    // says it is a placeholder, and carries the residency the engine actually knows beside it.
    j["host_kv"] = {
        {"net_present", false},
        {"net_absent_reason",
         "this build has no host-KV safety net; the flat counters and tier_census below are "
         "placeholders — read memory.host_kv_occupied_bytes and pressure.spill_pages instead"},
        {"residency",
         {{"occupied_bytes", m.host_kv_occupied_bytes},
          {"capacity_bytes", m.host_kv_capacity_bytes},
          {"spilled_pages", s.pressure_spill_pages},
          {"owners_demoted_kv", s.pressure_private_owners_demoted_kv},
          {"owners_demoted_kv_only", s.pressure_private_owners_demoted_kv_only}}},
        {"net_entries", 0},
        {"net_state_bytes", 0},
        {"superseded", 0},
        {"evictions", 0},
        {"compactions", 0},
        {"single_alloc_failures", 0},
        {"tier_census",
         {{"dead", {{"entries", 0}, {"bytes", 0}}},
          {"live", {{"entries", 0}, {"bytes", 0}}},
          {"idle", {{"entries", 0}, {"bytes", 0}}},
          {"active", {{"entries", 0}, {"bytes", 0}}}}},
        {"top_units", json::array()},
    };

    // ***THE REFUSAL REASON SPLIT, DERIVED from `host_kv_block_reason_name` rather than hand-typed -- and
    // written AFTER the `memory` assignment above ON PURPOSE.** An earlier change in this file set keys on
    // `j["memory"]` BEFORE `j["memory"] = {...}` and the assignment WIPED them: the deploy check passed,
    // the strings were in the binary, and the keys were absent from `/stats`. A key written before the
    // object it belongs to is replaced does not exist.
    // Read these against `host_kv_blocked_checks`, which is their denominator; the veto takes the split in
    // the tri-state call, not in the simulation both queries share.
    for (std::size_t reason = 0; reason < ninfer::k_host_kv_block_reason_count; ++reason) {
        const auto block_reason = static_cast<ninfer::HostKVBlockReason>(reason);
        j["memory"][std::string("host_kv_block_") + ninfer::host_kv_block_reason_name(block_reason)] =
            m.host_kv_block_reasons[reason];
    }

    j["load"] = {
        {"model_id", l.model_name},
        {"architecture", l.architecture},
        {"load_seconds", l.load_seconds},
    };

    j["cache_options"] = {
        {"enabled", cache.enabled},
        {"host_state_slots", cache.host_state_slots},
        {"host_kv_capacity_bytes", cache.host_kv_capacity_bytes},
    };

    return j.dump();
}

} // namespace ninfer::serve
