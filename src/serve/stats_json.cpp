#include "serve/stats_json.h"

#include <nlohmann/json.hpp>

namespace ninfer::serve {

namespace {
using nlohmann::json;

json arena(const ninfer::ArenaMemorySummary& a) {
    return json{{"capacity_bytes", a.capacity_bytes},
                {"used_bytes", a.used_bytes},
                {"peak_used_bytes", a.peak_used_bytes}};
}
} // namespace

std::string format_stats_json(const StatsSnapshot& s) {
    const json out = json{
        {"schema", "ninfer_serve_stats"},
        {"schema_version", 1},
        {"timestamp_unix_ms", s.timestamp_unix_ms},
        {"scheduler",
         json{{"running", s.scheduler.running_requests},
              {"prefilling", s.scheduler.prefilling_requests},
              {"decode_ready", s.scheduler.decode_ready_requests},
              {"waiting", s.scheduler.waiting_requests},
              {"materializing", s.scheduler.materializing_requests},
              {"capture_pending", s.scheduler.capture_pending_requests},
              {"terminal_pending", s.scheduler.terminal_pending_requests}}},
        {"counters",
         json{{"computed_prefill_tokens", s.scheduler.computed_prefill_tokens},
              {"committed_decode_tokens", s.scheduler.committed_decode_tokens},
              {"decode_rounds", s.scheduler.decode_rounds},
              {"decode_row_rounds", s.scheduler.decode_row_rounds},
              {"active_captures_completed", s.scheduler.active_captures_completed},
              {"active_captures_aborted", s.scheduler.active_captures_aborted}}},
        {"http", json{{"in_flight", s.in_flight}, {"max_in_flight", s.max_in_flight}}},
        {"kv_transfers",
         json{{"main_kv_d2h_pages", s.scheduler.main_kv_d2h_pages},
              {"main_kv_h2d_pages", s.scheduler.main_kv_h2d_pages},
              {"main_kv_d2d_pages", s.scheduler.main_kv_d2d_pages},
              {"main_kv_d2h_bytes", s.scheduler.main_kv_d2h_bytes},
              {"main_kv_h2d_bytes", s.scheduler.main_kv_h2d_bytes},
              {"main_kv_d2h_seconds", s.scheduler.main_kv_d2h_seconds},
              {"main_kv_h2d_seconds", s.scheduler.main_kv_h2d_seconds},
              {"backend_kv_d2h_pages", s.scheduler.backend_kv_d2h_pages},
              {"backend_kv_h2d_pages", s.scheduler.backend_kv_h2d_pages},
              {"backend_kv_d2h_bytes", s.scheduler.backend_kv_d2h_bytes},
              {"backend_kv_h2d_bytes", s.scheduler.backend_kv_h2d_bytes},
              {"backend_kv_d2h_seconds", s.scheduler.backend_kv_d2h_seconds},
              {"backend_kv_h2d_seconds", s.scheduler.backend_kv_h2d_seconds}}},
        {"state_transfers",
         json{{"state_d2h_count", s.scheduler.state_d2h_count},
              {"state_h2d_count", s.scheduler.state_h2d_count},
              {"state_d2d_count", s.scheduler.state_d2d_count},
              {"state_d2h_bytes", s.scheduler.state_d2h_bytes},
              {"state_h2d_bytes", s.scheduler.state_h2d_bytes},
              {"state_d2h_seconds", s.scheduler.state_d2h_seconds},
              {"state_h2d_seconds", s.scheduler.state_h2d_seconds}}},
        {"pressure",
         json{{"spill_pages", s.scheduler.pressure_spill_pages},
              {"private_owners_degraded", s.scheduler.pressure_private_owners_degraded},
              {"private_owners_evicted", s.scheduler.pressure_private_owners_evicted},
              {"shared_owners_degraded", s.scheduler.pressure_shared_owners_degraded},
              {"shared_owners_evicted", s.scheduler.pressure_shared_owners_evicted},
              {"checkpoints_dropped", s.scheduler.pressure_checkpoints_dropped},
              {"searches", s.scheduler.pressure_searches},
              {"search_budget_exhaustions", s.scheduler.pressure_search_budget_exhaustions},
              {"maximal_fallback_selections", s.scheduler.pressure_maximal_fallback_selections},
              {"admission_catalog_hits", s.scheduler.admission_catalog_hits},
              {"admission_safety_net_restores", s.scheduler.admission_safety_net_restores},
              {"device_main_kv_occupied_pages", s.scheduler.device_main_kv_occupied_pages},
              {"device_backend_kv_occupied_pages", s.scheduler.device_backend_kv_occupied_pages},
              {"device_state_occupied_slots", s.scheduler.device_state_occupied_slots}}},
        {"cache_reuse",
         json{{"root_selections", s.scheduler.root_selections},
              {"private_endpoint_selections", s.scheduler.private_endpoint_selections},
              {"private_turn_closure_selections", s.scheduler.private_turn_closure_selections},
              {"private_response_replay_selections", s.scheduler.private_response_replay_selections},
              {"private_long_anchor_selections", s.scheduler.private_long_anchor_selections},
              {"shared_stable_prefix_selections", s.scheduler.shared_stable_prefix_selections},
              {"reused_prompt_tokens", s.scheduler.reused_prompt_tokens},
              {"last_selected_frontier_tokens", s.scheduler.last_selected_frontier_tokens}}},
        {"memory",
         json{{"kv_capacity_page_groups", s.memory.kv_capacity_page_groups},
              {"kv_capacity_max_page_groups", s.memory.kv_capacity_max_page_groups},
              {"kv_payload_bytes", s.memory.kv_payload_bytes},
              {"planned_slack_bytes", s.memory.planned_slack_bytes},
              {"host_kv_capacity_bytes", s.memory.host_kv_capacity_bytes},
              {"host_kv_occupied_bytes", s.memory.host_kv_occupied_bytes},
              {"host_state_capacity_slots", s.memory.host_state_capacity_slots},
              {"host_state_occupied_slots", s.memory.host_state_occupied_slots},
              {"weights", arena(s.memory.weights)},
              {"sequence", arena(s.memory.sequence)},
              {"workspace", arena(s.memory.workspace)}}},
        {"load",
         json{{"target", s.load.target},
              {"model_id", s.load.model_id},
              {"weights_id", s.load.weights_id},
              {"load_seconds", s.load.load_seconds},
              {"tensor_count", s.load.tensor_count}}},
    };
    return out.dump();
}

} // namespace ninfer::serve
