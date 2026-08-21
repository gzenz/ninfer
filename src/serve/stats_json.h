#pragma once

// Read-only /stats payload: a boundary-consistent snapshot of scheduler,
// cumulative counters, HTTP admission depth, live KV occupancy, static memory
// layout, and load provenance. The formatter is a pure function (no I/O, no
// clock) so it is unit-testable; the HTTP handler fills timestamp_unix_ms and
// the live fields before calling it.

#include <ninfer/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ninfer::serve {

struct StatsSnapshot {
    std::uint64_t timestamp_unix_ms = 0;
    ninfer::RuntimeStats scheduler;
    std::size_t in_flight      = 0;
    std::size_t max_in_flight  = 0;
    ninfer::KvCacheStats kv_cache;
    ninfer::MemorySummary memory;
    ninfer::LoadSummary load;
};

std::string format_stats_json(const StatsSnapshot& snapshot);

} // namespace ninfer::serve