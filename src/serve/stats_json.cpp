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

json pool(const std::uint32_t page_groups, const std::uint32_t entitled,
         const std::uint32_t mapped, const std::uint32_t free) {
    return json{{"page_groups", page_groups},
                {"entitled_pages", entitled},
                {"mapped_pages", mapped},
                {"free_pages", free}};
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
              {"waiting", s.scheduler.waiting_requests}}},
        {"counters",
         json{{"computed_prefill_tokens", s.scheduler.computed_prefill_tokens},
              {"committed_decode_tokens", s.scheduler.committed_decode_tokens},
              {"decode_rounds", s.scheduler.decode_rounds},
              {"decode_row_rounds", s.scheduler.decode_row_rounds}}},
        {"http", json{{"in_flight", s.in_flight}, {"max_in_flight", s.max_in_flight}}},
        {"kv_cache",
         json{{"text", pool(s.kv_cache.text_page_groups, s.kv_cache.text_entitled_pages,
                            s.kv_cache.text_mapped_pages, s.kv_cache.text_free_pages)},
              {"mtp", pool(s.kv_cache.mtp_page_groups, s.kv_cache.mtp_entitled_pages,
                           s.kv_cache.mtp_mapped_pages, s.kv_cache.mtp_free_pages)},
              {"host",
               json{{"enabled", s.kv_cache.host_enabled},
                    {"budget_bytes", s.kv_cache.host_budget_bytes},
                    {"used_bytes", s.kv_cache.host_used_bytes},
                    {"largest_free_range", s.kv_cache.host_largest_free_range},
                    {"entries", s.kv_cache.host_entries}}}}},
        {"memory",
         json{{"kv_capacity_page_groups", s.memory.kv_capacity_page_groups},
              {"kv_capacity_max_page_groups", s.memory.kv_capacity_max_page_groups},
              {"kv_payload_bytes", s.memory.kv_payload_bytes},
              {"planned_slack_bytes", s.memory.planned_slack_bytes},
              {"weights", arena(s.memory.weights)},
              {"sequence", arena(s.memory.sequence)},
              {"workspace", arena(s.memory.workspace)},
              {"request_transient", arena(s.memory.request_transient)}}},
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