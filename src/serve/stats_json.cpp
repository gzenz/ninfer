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
        {"memory",
         json{{"kv_capacity_page_groups", s.memory.kv_capacity_page_groups},
              {"kv_capacity_max_page_groups", s.memory.kv_capacity_max_page_groups},
              {"kv_payload_bytes", s.memory.kv_payload_bytes},
              {"planned_slack_bytes", s.memory.planned_slack_bytes},
              {"host_kv_capacity_bytes", s.memory.host_kv_capacity_bytes},
              {"host_kv_occupied_bytes", s.memory.host_kv_occupied_bytes},
              {"device_state_occupied_slots", s.memory.device_state_occupied_slots},
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
