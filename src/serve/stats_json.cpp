#include "serve/stats_json.h"

#include <nlohmann/json.hpp>

namespace ninfer::serve {

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
        {"device_backend_kv_occupied_pages", s.device_backend_kv_occupied_pages},
        {"device_state_occupied_slots", s.device_state_occupied_slots},
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
        {"private_evictions_demotable", s.pressure_private_evictions_demotable},
        {"evictions_with_victim_room", s.pressure_evictions_with_victim_room},
        {"private_eviction_checks", s.pressure_private_eviction_checks},
        {"options", s.pressure_options},
        {"demote_options", s.pressure_demote_options},
        {"publication_cell_losses", s.pressure_publication_cell_losses},
        // The denominator beside it: goal-probe calls, not an outcome. See the RuntimeStats comment.
        {"publication_cell_probes", s.pressure_publication_cell_probes},
        // Uncapped, because the journal line that reports these is rate-limited to 8 then every 512th.
        {"publication_cell_at_risk_runs", s.pressure_publication_cell_at_risk_runs},
        {"publication_cell_veto_goals", s.pressure_publication_cell_veto_goals},
        {"publication_cell_veto_other", s.pressure_publication_cell_veto_other},
        {"publication_cell_veto_reuse", s.pressure_publication_cell_veto_reuse},
        {"checkpoints_dropped", s.pressure_checkpoints_dropped},
        {"searches", s.pressure_searches},
        {"search_budget_exhaustions", s.pressure_search_budget_exhaustions},
        {"maximal_fallback_selections", s.pressure_maximal_fallback_selections},
        {"shared_active_references", s.shared_active_references},
        {"actual_context_transfer_seconds", s.actual_context_transfer_seconds},
    };

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
        // From `s`, not `m`: the catalog lives in the ResourceManager (runtime) and the MemorySummary is
        // assembled by ProgramImpl, which cannot see it. Kept in this block anyway so the capacity/occupancy
        // pairs read together.
        {"private_catalog_capacity_cells", s.private_catalog_capacity_cells},
        {"catalog_cell_clears_terminal", s.catalog_cell_clears_terminal},
        {"catalog_cell_clears_action", s.catalog_cell_clears_action},
        {"catalog_cell_clears_cancelled", s.catalog_cell_clears_cancelled},
        {"catalog_cell_clears_cleanup", s.catalog_cell_clears_cleanup},
        {"catalog_cell_clears_rollback", s.catalog_cell_clears_rollback},
        {"private_catalog_occupied_cells", s.private_catalog_occupied_cells},
        {"host_pinned_capacity_bytes", m.host_pinned_capacity_bytes},
        {"host_pinned_free_bytes", m.host_pinned_free_bytes},
        {"host_pinned_chunks", m.host_pinned_chunks},
        {"host_pinned_grows", m.host_pinned_grows},
        {"host_pinned_grow_refusals", m.host_pinned_grow_refusals},
        {"host_state_pregrow_attempts", m.host_state_pregrow_attempts},
        {"host_state_pregrows", m.host_state_pregrows},
        {"host_state_pregrow_refusals", m.host_state_pregrow_refusals},
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
