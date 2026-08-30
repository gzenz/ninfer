#include "serve/stats_json.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

using Json = nlohmann::json;
using ninfer::serve::StatsSnapshot;

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) { return; }
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

StatsSnapshot make_snapshot() {
    StatsSnapshot s;
    s.timestamp_unix_ms = 1787344057488ULL;
    s.scheduler.running_requests      = 1;
    s.scheduler.prefilling_requests   = 1;
    s.scheduler.decode_ready_requests = 0;
    s.scheduler.waiting_requests      = 3;
    s.scheduler.materializing_requests = 2;
    s.scheduler.capture_pending_requests = 1;
    s.scheduler.terminal_pending_requests = 0;
    s.scheduler.computed_prefill_tokens = 123456789ULL;
    s.scheduler.committed_decode_tokens = 2345678ULL;
    s.scheduler.decode_rounds         = 123456ULL;
    s.scheduler.decode_row_rounds     = 124000ULL;
    s.scheduler.active_captures_completed = 500;
    s.scheduler.active_captures_aborted   = 3;
    s.in_flight     = 2;
    s.max_in_flight = 17;
    s.memory.kv_capacity_page_groups = 4096;
    s.memory.kv_capacity_max_page_groups = 8192;
    s.memory.kv_payload_bytes        = 1234567890ULL;
    s.memory.planned_slack_bytes     = 98765432ULL;
    s.memory.host_kv_capacity_bytes  = 8589934592ULL;
    s.memory.host_kv_occupied_bytes  = 4294967296ULL;
    s.memory.host_state_capacity_slots = 8;
    s.memory.host_state_occupied_slots   = 2;
    s.memory.weights.capacity_bytes  = 1000000;
    s.memory.weights.used_bytes      = 900000;
    s.memory.weights.peak_used_bytes = 950000;
    s.memory.sequence.capacity_bytes = 2000000;
    s.memory.sequence.used_bytes     = 1500000;
    s.memory.sequence.peak_used_bytes = 1800000;
    s.memory.workspace.capacity_bytes = 3000000;
    s.memory.workspace.used_bytes     = 2500000;
    s.memory.workspace.peak_used_bytes = 2800000;
    s.load.target        = "qwen3.8-27b";
    s.load.model_id      = "qwen3.8-27b";
    s.load.weights_id    = "nvfp4";
    s.load.load_seconds  = 3.9;
    s.load.tensor_count  = 721;
    return s;
}

void assert_common(const Json& doc) {
    check(doc["schema"] == "ninfer_serve_stats", "schema name");
    check(doc["schema_version"] == 1, "schema version");
    check(doc["timestamp_unix_ms"] == 1787344057488ULL, "timestamp");
    // scheduler
    check(doc["scheduler"]["running"] == 1, "scheduler.running");
    check(doc["scheduler"]["prefilling"] == 1, "scheduler.prefilling");
    check(doc["scheduler"]["decode_ready"] == 0, "scheduler.decode_ready");
    check(doc["scheduler"]["waiting"] == 3, "scheduler.waiting");
    check(doc["scheduler"]["materializing"] == 2, "scheduler.materializing");
    check(doc["scheduler"]["capture_pending"] == 1, "scheduler.capture_pending");
    check(doc["scheduler"]["terminal_pending"] == 0, "scheduler.terminal_pending");
    // counters
    check(doc["counters"]["computed_prefill_tokens"] == 123456789ULL, "counters.prefill");
    check(doc["counters"]["committed_decode_tokens"] == 2345678ULL, "counters.decode");
    check(doc["counters"]["decode_rounds"] == 123456ULL, "counters.rounds");
    check(doc["counters"]["decode_row_rounds"] == 124000ULL, "counters.row_rounds");
    check(doc["counters"]["active_captures_completed"] == 500, "counters.captures_completed");
    check(doc["counters"]["active_captures_aborted"] == 3, "counters.captures_aborted");
    // http
    check(doc["http"]["in_flight"] == 2, "http.in_flight");
    check(doc["http"]["max_in_flight"] == 17, "http.max_in_flight");
    // memory
    check(doc["memory"]["kv_capacity_page_groups"] == 4096, "memory.page_groups");
    check(doc["memory"]["kv_capacity_max_page_groups"] == 8192, "memory.max_page_groups");
    check(doc["memory"]["kv_payload_bytes"] == 1234567890ULL, "memory.kv_payload_bytes");
    check(doc["memory"]["planned_slack_bytes"] == 98765432ULL, "memory.slack");
    check(doc["memory"]["host_kv_capacity_bytes"] == 8589934592ULL, "memory.host_kv_capacity");
    check(doc["memory"]["host_kv_occupied_bytes"] == 4294967296ULL, "memory.host_kv_occupied");
    check(doc["memory"]["host_state_capacity_slots"] == 8, "memory.host_state_capacity_slots");
    check(doc["memory"]["host_state_occupied_slots"] == 2, "memory.host_state_occupied");
    check(doc["memory"].contains("weights"), "memory.weights present");
    check(doc["memory"].contains("sequence"), "memory.sequence present");
    check(doc["memory"].contains("workspace"), "memory.workspace present");
    check(doc["memory"]["weights"]["capacity_bytes"] == 1000000, "memory.weights.capacity");
    // load
    check(doc["load"]["target"] == "qwen3.8-27b", "load.target");
    check(doc["load"]["model_id"] == "qwen3.8-27b", "load.model_id");
    check(doc["load"]["weights_id"] == "nvfp4", "load.weights_id");
    check(doc["load"]["tensor_count"] == 721, "load.tensor_count");
}

} // namespace

int main() {
    // Populated snapshot: every field verified.
    {
        const Json doc = Json::parse(ninfer::serve::format_stats_json(make_snapshot()));
        assert_common(doc);
    }
    // Default-constructed snapshot: all zeros, still well-formed JSON.
    {
        const Json doc = Json::parse(ninfer::serve::format_stats_json(StatsSnapshot{}));
        check(!doc.contains("kv_cache"), "no kv_cache key (upstream has no KvCacheStats)");
        check(doc["scheduler"]["running"] == 0, "default scheduler zero");
        check(doc["http"]["in_flight"] == 0, "default http zero");
    }
    if (failures == 0) {
        std::cout << "test_stats_json: all checks passed\n";
        return 0;
    }
    std::cerr << "test_stats_json: " << failures << " check(s) failed\n";
    return 1;
}
