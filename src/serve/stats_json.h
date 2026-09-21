#pragma once

#include <string>

#include <ninfer/types.h>

namespace ninfer::serve {

// Serializes the engine's RuntimeStats + MemorySummary + LoadSummary into the
// JSON shape our /stats consumers expect (dashboard, e2e StatsPoller, journal
// grep). kv_transfers/state_transfers use the FLAT key names the monitor reads.
// Upstream has no host-KV net, so the `host_kv` block is present but all-zero
// (no tier_census / top_units / net counters).
std::string format_stats_json(const ninfer::RuntimeStats& stats,
                              const ninfer::MemorySummary& memory,
                              const ninfer::LoadSummary& load,
                              const ninfer::ContextCacheOptions& cache_options,
                              std::size_t http_in_flight  = 0,
                              std::size_t http_max_in_flight = 0);

} // namespace ninfer::serve
