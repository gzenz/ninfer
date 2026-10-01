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

#include <array>
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

// WHY a growth attempt was refused. `decide()` compared four things and returned one bool, so a refusal
// could not be told from the others -- and on 2026-10-01 the reason had to be DERIVED by reading this file
// against the running argv, which is exactly the class of conclusion this repo keeps having to retract.
//   `Reserve`        -- MemAvailable minus the reserve could not take the chunk. The RAM gate, and on a
//                       config with no `--host-pinned-max-mib` it is the only one that can fire.
//   `MaxBytes`       -- the configured pinned ceiling. A CONFIG fact, not a machine one.
//   `ShmemCap`       -- the optional shmem ceiling (0 today, so unreachable).
//   `InvalidReading` -- /proc/meminfo could not be read or parsed. FAIL CLOSED, and it looks identical to
//                       "the machine is full" in every counter: distinct here for that reason.
//   `NothingWanted`  -- bytes == 0, i.e. no attempt was really made.
enum class GrowthVeto : std::uint8_t {
    None = 0,
    NothingWanted,
    InvalidReading,
    Reserve,
    MaxBytes,
    ShmemCap,
};

[[nodiscard]] inline constexpr const char* growth_veto_name(GrowthVeto veto) noexcept {
    switch (veto) {
    case GrowthVeto::None:           return "none";
    case GrowthVeto::NothingWanted:  return "nothing_wanted";
    case GrowthVeto::InvalidReading: return "invalid_reading";
    case GrowthVeto::Reserve:        return "reserve";
    case GrowthVeto::MaxBytes:       return "max_bytes";
    case GrowthVeto::ShmemCap:       return "shmem_cap";
    }
    return "none";
}

class HostMemoryBudget {
public:
    explicit HostMemoryBudget(HostMemoryBudgetConfig config) noexcept : config_(config) {}

    // Decide from a reading, with no side effects: the testable core.
    [[nodiscard]] bool decide(const HostMemoryReading& reading, std::size_t bytes) const noexcept;

    // The same decision, saying WHY. `decide()` is this plus a `== None`.
    [[nodiscard]] GrowthVeto veto_for(const HostMemoryReading& reading,
                                     std::size_t bytes) const noexcept;

    // Refresh the reading, then decide. Used as the pool's growth policy, so it must be cheap and must
    // never grow anything itself.
    [[nodiscard]] bool allow(std::size_t bytes) noexcept;

    // The SAME bookkeeping on an INJECTED reading. This exists so the refusal branch can be tested AT ALL:
    // `allow()` reads /proc/meminfo, so on any healthy machine it approves, and a test that means to
    // exercise "refuse, then approve, and the snapshot must not follow the approval" cannot force the
    // refusal. The first version of that test wrote `check(!allow(...) || true)` -- a tautology that passed
    // whatever the code did, and a mutation reintroducing the very bug it guarded survived 5 of 5 runs.
    // `allow()` is now this plus `read_host_memory()`, so there is one bookkeeping site and two entry points.
    [[nodiscard]] bool allow_with(const HostMemoryReading& reading, std::size_t bytes) noexcept;

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

    // The reason, and the READING IT WAS TAKEN FROM. The last reading alone answers the question the total
    // cannot: "refused" plus `mem_available`/`reserve` says whether the machine was short or the policy was
    // strict, and `invalid` says the fail-closed path fired -- a state that otherwise hides inside the same
    // counter as a full machine.
    [[nodiscard]] GrowthVeto last_veto() const noexcept { return last_veto_; }
    [[nodiscard]] std::size_t last_wanted_bytes() const noexcept { return last_wanted_bytes_; }
    // THE READING THE VETO WAS TAKEN FROM -- snapshotted at the refusal, NOT read live. `reading_` is
    // refreshed on every `allow()`, approvals included, so exporting `reading().mem_available` beside
    // `last_veto()` shows a LATER reading than the one that produced the refusal whenever any growth has
    // been approved since -- which is exactly the case a reader is trying to interpret ("was the machine
    // short, or was the policy strict?"), so the misleading version is worse than none.
    [[nodiscard]] std::size_t last_veto_mem_available() const noexcept {
        return last_veto_mem_available_;
    }

private:
    HostMemoryBudgetConfig config_;
    HostMemoryReading      reading_;
    std::size_t            pinned_bytes_ = 0U;
    std::uint64_t          refusals_     = 0U;
    std::uint64_t          approvals_    = 0U;
    GrowthVeto             last_veto_    = GrowthVeto::None;
    std::size_t            last_wanted_bytes_ = 0U;
    std::size_t            last_veto_mem_available_ = 0U;
};

}  // namespace ninfer
