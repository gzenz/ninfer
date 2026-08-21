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

StatsSnapshot make_snapshot(bool host_enabled) {
    StatsSnapshot s;
    s.timestamp_unix_ms = 1787344057488ULL;
    s.scheduler.running_requests      = 1;
    s.scheduler.prefilling_requests   = 1;
    s.scheduler.decode_ready_requests = 0;
    s.scheduler.waiting_requests      = 3;
    s.scheduler.computed_prefill_tokens = 123456789ULL;
    s.scheduler.committed_decode_tokens = 2345678ULL;
    s.scheduler.decode_rounds         = 123456ULL;
    s.scheduler.decode_row_rounds     = 124000ULL;
    s.in_flight     = 2;
    s.max_in_flight = 17;
    s.kv_cache.text_page_groups    = 4096;
    s.kv_cache.text_entitled_pages = 3812;
    s.kv_cache.text_mapped_pages   = 3600;
    s.kv_cache.text_free_pages     = 284;
    s.kv_cache.mtp_page_groups    = 512;
    s.kv_cache.mtp_entitled_pages = 476;
    s.kv_cache.mtp_mapped_pages   = 450;
    s.kv_cache.mtp_free_pages     = 62;
    s.kv_cache.host_enabled    = host_enabled;
    s.kv_cache.host_slabs      = host_enabled ? 3 : 0;
    s.kv_cache.host_free_slabs = host_enabled ? 1 : 0;
    s.kv_cache.host_entries    = host_enabled ? 2 : 0;
    s.kv_cache.host_slab_bytes = host_enabled ? 3100000000ULL : 0;
    s.memory.kv_capacity_page_groups = 4096;
    s.memory.kv_payload_bytes        = 1234567890ULL;
    s.memory.planned_slack_bytes     = 98765432ULL;
    s.load.target        = "qwen3.8-27b";
    s.load.model_id      = "qwen3.8-27b";
    s.load.weights_id    = "nvfp4";
    s.load.load_seconds  = 3.9;
    s.load.tensor_count  = 721;
    return s;
}

void assert_common(const Json& doc, bool host_enabled) {
    check(doc["schema"] == "ninfer_serve_stats", "schema name");
    check(doc["schema_version"] == 1, "schema version");
    check(doc["timestamp_unix_ms"] == 1787344057488ULL, "timestamp");
    // scheduler (snapshot half of RuntimeStats)
    check(doc["scheduler"]["running"] == 1, "scheduler.running");
    check(doc["scheduler"]["prefilling"] == 1, "scheduler.prefilling");
    check(doc["scheduler"]["decode_ready"] == 0, "scheduler.decode_ready");
    check(doc["scheduler"]["waiting"] == 3, "scheduler.waiting");
    // counters (cumulative half of RuntimeStats)
    check(doc["counters"]["computed_prefill_tokens"] == 123456789ULL, "counters.prefill");
    check(doc["counters"]["committed_decode_tokens"] == 2345678ULL, "counters.decode");
    check(doc["counters"]["decode_rounds"] == 123456ULL, "counters.rounds");
    check(doc["counters"]["decode_row_rounds"] == 124000ULL, "counters.row_rounds");
    // http
    check(doc["http"]["in_flight"] == 2, "http.in_flight");
    check(doc["http"]["max_in_flight"] == 17, "http.max_in_flight");
    // kv_cache text pool
    check(doc["kv_cache"]["text"]["page_groups"] == 4096, "kv.text.page_groups");
    check(doc["kv_cache"]["text"]["entitled_pages"] == 3812, "kv.text.entitled");
    check(doc["kv_cache"]["text"]["mapped_pages"] == 3600, "kv.text.mapped");
    check(doc["kv_cache"]["text"]["free_pages"] == 284, "kv.text.free");
    // kv_cache mtp pool
    check(doc["kv_cache"]["mtp"]["page_groups"] == 512, "kv.mtp.page_groups");
    check(doc["kv_cache"]["mtp"]["mapped_pages"] == 450, "kv.mtp.mapped");
    // kv_cache host
    check(doc["kv_cache"]["host"]["enabled"] == host_enabled, "kv.host.enabled");
    if (host_enabled) {
        check(doc["kv_cache"]["host"]["slabs"] == 3, "kv.host.slabs");
        check(doc["kv_cache"]["host"]["free_slabs"] == 1, "kv.host.free_slabs");
        check(doc["kv_cache"]["host"]["entries"] == 2, "kv.host.entries");
        check(doc["kv_cache"]["host"]["slab_bytes"] == 3100000000ULL, "kv.host.slab_bytes");
    } else {
        check(doc["kv_cache"]["host"]["slabs"] == 0, "kv.host.slabs zero when disabled");
        check(doc["kv_cache"]["host"]["entries"] == 0, "kv.host.entries zero when disabled");
        check(doc["kv_cache"]["host"]["slab_bytes"] == 0, "kv.host.slab_bytes zero when disabled");
    }
    // memory
    check(doc["memory"]["kv_capacity_page_groups"] == 4096, "memory.page_groups");
    check(doc["memory"]["kv_payload_bytes"] == 1234567890ULL, "memory.kv_payload_bytes");
    check(doc["memory"]["planned_slack_bytes"] == 98765432ULL, "memory.slack");
    check(doc["memory"].contains("weights"), "memory.weights present");
    check(doc["memory"].contains("sequence"), "memory.sequence present");
    check(doc["memory"].contains("workspace"), "memory.workspace present");
    check(doc["memory"].contains("request_transient"), "memory.request_transient present");
    // load
    check(doc["load"]["target"] == "qwen3.8-27b", "load.target");
    check(doc["load"]["model_id"] == "qwen3.8-27b", "load.model_id");
    check(doc["load"]["weights_id"] == "nvfp4", "load.weights_id");
    check(doc["load"]["tensor_count"] == 721, "load.tensor_count");
}

} // namespace

int main() {
    // Host-enabled case: every field populated.
    {
        const Json doc = Json::parse(ninfer::serve::format_stats_json(make_snapshot(true)));
        assert_common(doc, /*host_enabled=*/true);
    }
    // Host-disabled case: host fields are present but zeroed.
    {
        const Json doc = Json::parse(ninfer::serve::format_stats_json(make_snapshot(false)));
        assert_common(doc, /*host_enabled=*/false);
    }
    // Default-constructed snapshot: all zeros, still well-formed JSON.
    {
        const Json doc = Json::parse(ninfer::serve::format_stats_json(StatsSnapshot{}));
        check(doc["kv_cache"]["text"]["page_groups"] == 0, "default text pool zero");
        check(doc["kv_cache"]["host"]["enabled"] == false, "default host disabled");
        check(doc["scheduler"]["running"] == 0, "default scheduler zero");
    }
    if (failures == 0) {
        std::cout << "test_stats_json: all checks passed\n";
        return 0;
    }
    std::cerr << "test_stats_json: " << failures << " check(s) failed\n";
    return 1;
}