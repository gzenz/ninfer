// The growth policy for the elastic pinned host pool: how much more may we pin, right now?
//
// This exists because "no fixed ceiling" must not become this host's documented failure mode, which is
// pinned shmem (53 GB RAM, prod peaks ~34.7 G, 16 GB of swap never used). The ceiling is the host's RAM
// minus a reserve, evaluated at each growth attempt rather than fixed at startup.
//
// THE GATE IS `MemAvailable` MINUS THE RESERVE, and only that. `Shmem:` is read because the e2e harness's
// VM-OOM guard polls it and it is the field this host's failure mode shows up in, but the engine passes
// `shmem_cap_bytes = 0` today, so it does NOT gate on it. (An earlier version of this comment claimed the
// engine's gate and the harness's guard "measure the same field", which the review of 2026-09-26 refuted by
// reading the call site: Shmem is parsed and unused.)
//
// FAIL CLOSED. An unreadable or malformed /proc/meminfo means `allow()` is false: the cost of refusing to
// grow is an eviction (bad), while the cost of growing blind is the VM going down with the operator's
// session in it (much worse). That asymmetry is the whole reason this is a separate, tested type.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ninfer {

struct HostMemoryReading {
    std::size_t mem_available = 0;
    std::size_t shmem         = 0;
    std::size_t swap_free     = 0;
    bool        valid         = false;  // false: could not be read or parsed, so nothing may be concluded
};

// Parse /proc/meminfo-formatted text. Separate from the reader so that malformed, truncated and absent
// input can be TESTED -- the fail-closed path is the one that matters and it cannot be exercised by
// reading a healthy machine.
[[nodiscard]] HostMemoryReading parse_meminfo(std::string_view text) noexcept;

// Read /proc/meminfo. `valid == false` on any failure.
[[nodiscard]] HostMemoryReading read_host_memory() noexcept;

// The engine supplies these from its options (`ContextCacheOptions::host_pinned_*`), so the number
// lives in exactly one place. This type's own default is 0, which means "no reserve" and is only
// reachable by a caller that does not care (a test).

struct HostMemoryBudgetConfig {
    std::size_t reserve_bytes   = 0U;  // RAM that pinned memory may never eat into. The safety property.
    std::size_t max_bytes       = 0U;  // 0 = no fixed ceiling beyond what the reserve leaves
    std::size_t shmem_cap_bytes = 0U;  // 0 = no separate shmem ceiling; else a hard stop on total pinned
};

class HostMemoryBudget {
public:
    explicit HostMemoryBudget(HostMemoryBudgetConfig config) noexcept : config_(config) {}

    // Decide from a reading, with no side effects: the testable core.
    [[nodiscard]] bool decide(const HostMemoryReading& reading, std::size_t bytes) const noexcept;

    // Refresh the reading, then decide. Used as the pool's growth policy, so it must be cheap and must
    // never grow anything itself.
    [[nodiscard]] bool allow(std::size_t bytes) noexcept;

    // What is already pinned, as reported by the pool (`capacity_bytes()`). Kept here rather than read from
    // the machine, because the pool is the only thing that knows what THIS process reserved.
    void set_pinned_bytes(std::size_t bytes) noexcept { pinned_bytes_ = bytes; }

    // The largest single request that would be allowed right now. 0 when nothing more may be pinned.
    [[nodiscard]] std::size_t max_pinnable() const noexcept;

    [[nodiscard]] const HostMemoryReading& reading() const noexcept { return reading_; }
    [[nodiscard]] const HostMemoryBudgetConfig& config() const noexcept { return config_; }

    // A refusal must be countable, or it is indistinguishable from a pool that never needed to grow --
    // the shape of blindness this repo keeps recording.
    [[nodiscard]] std::uint64_t refusals() const noexcept { return refusals_; }
    [[nodiscard]] std::uint64_t approvals() const noexcept { return approvals_; }

private:
    HostMemoryBudgetConfig config_;
    HostMemoryReading      reading_;
    std::size_t            pinned_bytes_ = 0U;
    std::uint64_t          refusals_     = 0U;
    std::uint64_t          approvals_    = 0U;
};

}  // namespace ninfer
