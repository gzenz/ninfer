#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ninfer {

// The per-victim eviction record is a RING; this is its capacity. Sized to cover a whole e2e window's
// eviction population (measured 61-85 evictions, 31-49 of them KV-bearing, 2026-10-04) so that a normal
// window does NOT wrap and `records_total == record_count` is the common case -- when it does wrap the two
// diverge and the reader is told rather than silently shown a suffix.
inline constexpr std::size_t k_eviction_kv_record_capacity = 128;

// ***THE SIX AXES THE FEASIBILITY CONJUNCTION ANDS, NAMED ONCE (declared here, above its first use).***
// The order is load-bearing: it indexes `axis_fits`, `refusal_mask`'s bits, the per-axis refusal counters and
// `AssessedPeakRecord::axis_shortfall`. The gate itself (`context.cpp`, `physical_peak_fits`) builds its
// `axis_fits` array in exactly this order and derives its return from it, so the two cannot drift.
inline constexpr std::size_t k_peak_axis_count = 6;


// ***ONE ASSESSMENT'S PER-AXIS ARITHMETIC, AS NAMED FIELDS (2026-10-07).***
// These four series are all `std::array<std::uint64_t, k_peak_axis_count>` in `k_peak_axis_names` order, so
// passed as four separate arguments a shuffled call would COMPILE and read plausibly -- which is exactly the
// argument-order defect this tree has already paid for once (`note_kv_relief`, arm A33). Grouping them makes
// the assignment positional no longer: the fields are named at the call site.
//   peak          -- `physical_peak_additional`, the plan's own demand
//   reservation   -- `reservation_added`, the plan's entitlement
//   removed       -- `final_removed`, what the plan takes away
struct PeakAxisSample {
    std::array<std::uint64_t, k_peak_axis_count> peak{};
    std::array<std::uint64_t, k_peak_axis_count> reservation{};
    std::array<std::uint64_t, k_peak_axis_count> removed{};
};

// ONE FEASIBILITY ASSESSMENT, AS IT ACTUALLY WAS. **The averages could not decide #6**: the MEAN reservation
// makes an EVICTION's own arithmetic fail too (`occupied 4095 + (2296 - 2041) > 4096`) while evictions ARE
// admitted -- so the mean is the wrong quantity and the individual triple is the right one. This records, per
// assessment: what the plan asked for (`peak_*`), what the pool held (`occupied_*`) and its ceiling, whether
// the target evicts, and whether it was admitted. Read it as evidence, per instance, not as a rate.
struct AssessedPeakRecord {
    std::uint32_t peak_main_kv       = 0;
    std::uint32_t peak_backend_kv    = 0;
    std::uint32_t occupied_main_kv   = 0;
    std::uint32_t occupied_backend_kv = 0;
    std::uint32_t capacity_main_kv   = 0;
    std::uint8_t  preserving         = 0;   // the target's own decisions carry no eviction
    std::uint8_t  feasible           = 0;
    // **WHY IT WAS REFUSED, WHICH FINDING 3 OF ARM A18 SHOWED WAS MISSING.** Several rows were refused with a
    // peak that FITS on main KV (7 against a slack of 7), so the refusal came from somewhere the record did
    // not name. Bits 0..(k_peak_axis_count-1) = the peak axes that failed; bit 7 = the host-allocation gate
    // (`blocked_host_allocation_bytes != 0`). Zero on an admitted row.
    std::uint8_t  refusal_mask       = 0;
    // **THE OTHER TWO TERMS OF THE ARITHMETIC, so `peak == max(0, reservation - removed)` is CHECKABLE per
    // instance.** Without them the ring showed that admitted plans have peak 7 and refused ones 2,490 but not
    // WHY -- a peak that is not the difference of these two would mean the model is wrong rather than the plan.
    std::uint32_t reservation_main_kv = 0;
    std::uint32_t removed_main_kv     = 0;
    // **HOW MUCH OF `removed` IS THE EVICTED VICTIM.** `final_removed` is seeded from the plan's SOURCE and
    // accumulates per step, so a total of 1,610 cannot be read as "the victim's pages" until the evicting
    // decisions' OWN `effect.removed` is summed out of it. That sum is this field: for an evicting decision,
    // `effect.removed` IS the victim's removal. Read it AGAINST `removed_main_kv`.
    std::uint32_t evicted_victim_main_kv = 0;
    // **HOW MANY VICTIMS THE PLAN ACTED ON, WHICH IS THE SEARCH QUESTION.** A25 showed admitted plans free
    // 2,990 pages -- more than one victim's 1,607 -- so the refused rows' ~1,400 relief is either ONE victim
    // freed and no more chosen, or several chosen that are each small. `selected_decisions` and
    // `evicting_decisions` separate those two, and they are the numbers the search's stopping point shows up in.
    std::uint32_t selected_decisions = 0;
    std::uint32_t evicting_decisions = 0;
    // **WAS A LARGER VICTIM AVAILABLE?** The plan's best CHOSEN relief against the best relief available in
    // `CandidateVictimOptions`. This is the last question before a change to the search: if `available` beats
    // `chosen` on the refused rows, the search had a victim that would have closed the deficit and did not
    // take it -- the remedy is the RANKING. If not, the largest victim available was still too small and the
    // remedy is the reservation or the pool.
    std::uint32_t best_available_main_kv = 0;
    std::uint32_t chosen_best_main_kv    = 0;
    // **IS A DEMOTE PRICED PER RUN?** `degradation_units` charges `decision.main_kv_changes.size()`, and
    // `add_kv` pushes ONE ENTRY PER CONTIGUOUS RUN -- so a large victim's demote would cost more degradation
    // units than a small one's, while an eviction carries no KV changes at all. That is the code-read
    // hypothesis for A28's asymmetry, and this is the measurement that settles it: change ENTRIES per row
    // against the pages those entries move.
    std::uint32_t selected_kv_change_entries = 0;
    // *** HOW FAR SHORT, ON EACH AXIS -- THE PER-AXIS MARGIN (2026-10-07). ***
    // `refusal_mask` says WHICH of the six axes killed a target, and `peak_main_kv` gives the magnitude on one
    // of them. Neither answers the question #6 ends on: at `slack = 0` admission needs `removed >=
    // reservation` on EVERY axis, so a preserving plan can be refused by an axis nobody measured while the
    // main-KV row looks healthy. This is, per axis and in the SAME ORDER the conjunction uses
    // (`k_peak_axis_names`),
    //     shortfall[axis] = max(0, occupied[axis] + peak[axis] - capacity[axis])
    // -- the number of units the axis misses by, which is the margin `peak_main_kv` gave for one axis only.
    // **IT IS CHECKABLE PER ROW AND THAT IS THE POINT:** `shortfall[a] > 0` must hold for exactly the axes
    // whose bit is set in `refusal_mask`, on a refused row, and all six must be zero on an admitted one. A row
    // that disagrees is an instrument defect, not a finding -- the failure this file has recorded three times.
    std::array<std::uint32_t, k_peak_axis_count> axis_shortfall{};
};
inline constexpr std::size_t k_assessed_peak_record_capacity = 64;

// ***WHY A PLACEMENT WAS REFUSED, NAMED ONE BY ONE (2026-10-05).*** `fit_after_suballocation_releases`
// returns `Blocked` with the SHORTFALL UNSET for both `!valid` and `descriptors_short`, so every such
// refusal looked identical and nothing could tell a DESCRIPTOR SHORTAGE (fix: grow the tables) from a
// MALFORMED RELEASE LIST (fix: the planner -- and no capacity setting can touch it). An observed window had
// 82 of 82 vetoes on this path, which is why the distinction was worth a counter: the two causes imply
// opposite work in opposite layers. FIVE returns produce it, and THREE are about the release list.
enum class HostKVBlockReason : std::uint8_t {
    None,                            // not a refusal (the empty case, or a normal end)
    ReleaseHandleInvalid,            // `!valid_handle || page_count == 0`      -- RELEASE list
    ReleaseRangeExceedsDescriptor,   // the range runs past its descriptor      -- RELEASE list
    ReleasesOverlap,                 // two releases overlap                    -- RELEASE list
    RequestLayoutMissing,            // `layout == nullptr || pages == 0`
    RequestLayoutUnknown,            // an unknown layout, or a stride overflow
    DescriptorsShort,                // the table cannot describe the placement
    // ***THE STORE'S THREE, AND THEY ARE THE ONES THAT ACTUALLY FIRE.*** `HostKVExtentStore::
    // fit_after_page_releases` returns `Blocked` with the shortfall UNSET at three places BEFORE it calls
    // the arena at all, so a split taken only in the arena counts NOTHING for them -- measured: the veto
    // fired 12 times and every arena-side reason stayed 0. BOTH are about the RELEASE LIST the planner
    // builds, and neither is reachable by a capacity setting.
    StoreReleasePagesNull,           // `release.pages == nullptr`
    StoreReleaseExtentUnallocated,   // the replica's extent has no allocation
    // ***`mark_release`'s EIGHT PREDICATES, SPLIT APART (2026-10-05).*** The single counter this replaces
    // was named `store_release_already_marked` AND COUNTED ALL EIGHT -- a name that claims one cause while
    // counting eight is worse than no counter, because a reader acts on the name. Every one of these means
    // `mark_release` refused the release, and which one refuses is the question.
    StoreMarkPageNotHostResident,    // `!pages.valid(page) || !pages.host_resident(page)`
    StoreMarkSourcePinned,           // `pages.source_pins(page) != 0`
    StoreMarkNotSoleReference,       // `pages.address_references(page) != 1` (last-reference path)
    StoreMarkCannotReleaseReplica,   // `!can_release_page_replica(pages, page)`
    StoreMarkReplicaInvalid,         // `!valid(replica.extent) || membership_node out of range`
    StoreMarkMembershipMismatch,     // the membership's page/extent/offset/store does not match the replica
    StoreMarkAlreadyMarked,          // the stamp: the SAME PAGE WAS ALREADY MARKED IN THIS CALL
    Count,
};
inline constexpr std::size_t k_host_kv_block_reason_count =
    static_cast<std::size_t>(HostKVBlockReason::Count);

[[nodiscard]] inline constexpr const char* host_kv_block_reason_name(HostKVBlockReason reason) noexcept {
    switch (reason) {
    case HostKVBlockReason::None:                          return "none";
    case HostKVBlockReason::ReleaseHandleInvalid:          return "release_handle_invalid";
    case HostKVBlockReason::ReleaseRangeExceedsDescriptor: return "release_range_exceeds_descriptor";
    case HostKVBlockReason::ReleasesOverlap:               return "releases_overlap";
    case HostKVBlockReason::RequestLayoutMissing:          return "request_layout_missing";
    case HostKVBlockReason::RequestLayoutUnknown:          return "request_layout_unknown";
    case HostKVBlockReason::DescriptorsShort:              return "descriptors_short";
    case HostKVBlockReason::StoreReleasePagesNull:         return "store_release_pages_null";
    case HostKVBlockReason::StoreMarkPageNotHostResident:  return "store_mark_page_not_host_resident";
    case HostKVBlockReason::StoreMarkSourcePinned:         return "store_mark_source_pinned";
    case HostKVBlockReason::StoreMarkNotSoleReference:     return "store_mark_not_sole_reference";
    case HostKVBlockReason::StoreMarkCannotReleaseReplica: return "store_mark_cannot_release_replica";
    case HostKVBlockReason::StoreMarkReplicaInvalid:       return "store_mark_replica_invalid";
    case HostKVBlockReason::StoreMarkMembershipMismatch:   return "store_mark_membership_mismatch";
    case HostKVBlockReason::StoreMarkAlreadyMarked:        return "store_mark_already_marked";
    case HostKVBlockReason::StoreReleaseExtentUnallocated: return "store_release_extent_unallocated";
    case HostKVBlockReason::Count:                         break;
    }
    return "none";
}

// ***WHICH HOST-RELEASE LIST A PAGE BELONGS IN, AND THE STORE'S PREDICATES ARE THE SPECIFICATION.***
// `HostKVExtentStore::fit_after_page_releases` applies a DIFFERENT predicate to each list:
//   PLAIN           -- `can_release_page_replica`: `device_resident || address_references == 0`
//                      i.e. a device copy remains, so dropping the host replica is safe;
//   LAST REFERENCE  -- `mark_release(..., last_reference = true)`: `address_references == 1`
//                      i.e. this release removes the ONLY address that used the page.
// A page that is HOST-ONLY and still SHARED satisfies NEITHER, and the store REFUSES it -- correctly,
// because releasing that replica would destroy a copy another address still uses.
//
// **THE DEFECT THIS EXISTS TO PREVENT (2026-10-05).** The planner pushed EVERY released host page into the
// last-reference list, so the store refused every shared page, `mark_release` failing made the WHOLE plan
// read `Blocked` with the shortfall UNSET -- before the arena was asked -- and EVERY preserving plan was
// rejected. The engine then evicted victims whose own KV the host arena would have taken. Measured on QA:
// 13 of 13 veto refusals were `store_mark_not_sole_reference`.
//
// DEVICE-RESIDENT WINS OVER THE REFERENCE COUNT when both predicates would accept: the true situation is
// "the host replica drops and a device copy remains", which is what PLAIN means.
enum class HostReleaseKind : std::uint8_t {
    NotReleased,    // host-only and still shared: NOT safe to offer, and no host bytes are freed
    Plain,          // a device copy remains
    LastReference,  // this release takes the page's only address reference
};

[[nodiscard]] constexpr HostReleaseKind classify_host_release(bool host_resident, bool device_resident,
                                                              std::uint32_t address_references) noexcept {
    if (!host_resident) { return HostReleaseKind::NotReleased; }
    if (device_resident) { return HostReleaseKind::Plain; }
    if (address_references == 1U) { return HostReleaseKind::LastReference; }
    return HostReleaseKind::NotReleased;
}

// THE SIX AXES `physical_peak_fits` ANDs, in the order that function checks them. Named here as the ONE
// source for both the counter array and the `/stats` keys, so a key cannot drift from the axis it counts --
// the same derivation `k_demote_refusal_names` uses. The order is load-bearing: `physical_peak_fits` builds
// its per-axis array in THIS order and derives its boolean from that array, so a reordering here silently
// renames every count. **No test asserts the order** (said plainly rather than implied) -- the guard is that
// the array and the boolean are the same object, so a mismatch would have to be introduced deliberately in
// two places at once.
// WHY A DEVICE-KV PAGE WAS REJECTED AS A RELIEF CANDIDATE, in the ORDER the eligibility test evaluates them.
// The order is load-bearing for the same reason `k_peak_axis_names`' is: the enum indexes the array, and the
// test short-circuits, so the FIRST failing clause is the one that blocks that page.
//
// **THIS EXISTS FOR #6's LAST QUESTION.** `select_kv_pressure_actions` builds a device-KV relief by scanning
// pages and rejecting the ineligible ones, and the relief available to a PRESERVING plan (a demote to host)
// differs from an eviction's, because the demote path refuses any page with an active address reference. One
// undifferentiated count cannot say which clause blocks which relief, and the candidate mechanism -- that
// `active_reference` is what makes every preserving plan infeasible on the device axes -- is exactly a claim
// about ONE of these clauses.
inline constexpr std::size_t k_kv_relief_clause_count = 7;
inline constexpr const char* k_kv_relief_clause_names[k_kv_relief_clause_count] = {
    "already_selected",
    "not_device_resident",
    "writer_references",
    "source_pins",
    "not_replica_safe",
    "active_reference",
    "protected",
};
// The two directions, in the order of the `rejected_by_clause` array's first index. A DROP keeps a host
// replica and therefore needs one to exist; a DEMOTE needs the page NOT host-resident. Different
// preconditions, so they are counted apart.
inline constexpr std::size_t k_kv_relief_direction_count = 2;
inline constexpr const char* k_kv_relief_direction_names[k_kv_relief_direction_count] = {
    "demote",
    "drop",
};
// WHY THE DEMOTE PASS DID NOT RUN, in the gate's own order (`select_kv_pressure_actions`). A demote that
// never ran is not a demote that found nothing, and the rejection histogram reads identically for the two.
//
// **`demand_already_zero` WAS NAMED `drop_satisfied_demand` FOR ONE RUN, AND THE SAME RUN REFUTED THE NAME:**
// it buckets the case where `device_remaining == 0` at the gate, and on arm A8 the DROP direction relieved
// **0** pages across 11,518 calls -- so the drop cannot have satisfied anything. What the bucket actually
// measures is "this call asked for no device relief", which is why it is named for its condition and not for
// a cause. The two are told apart by reading `kv_relief_relieved_drop` beside it.
inline constexpr std::size_t k_kv_relief_gate_clause_count = 3;
inline constexpr const char* k_kv_relief_gate_clause_names[k_kv_relief_gate_clause_count] = {
    "demand_already_zero",
    "host_bytes_short",
    "no_host_allocation",
};

// THE CLAUSES THAT CAN EMPTY A SUCCESSOR SET, in `SuccessorEmptyReason` order, named here as the ONE source
// for both the counter array and the `/stats` keys (the derivation `k_peak_axis_names` uses). The order is
// load-bearing for the same reason: the enum indexes the array. The first two are STRUCTURAL (no KV to act
// on; the current decision already evicts) and were 0 of 1609 on the #6 repro; the last two are the pair that
// must never be read as one number -- `no_endpoints` means the residual asked for NOTHING, `all_refused`
// means it asked and the option builder said no.
inline constexpr std::size_t k_successor_empty_reason_count = 5;
inline constexpr const char* k_successor_empty_reason_names[k_successor_empty_reason_count] = {
    "no_kv",
    "evicting_current",
    "unretained",
    "no_endpoints",
    "all_refused",
};

inline constexpr const char* k_peak_axis_names[k_peak_axis_count] = {
    "device_active_lanes",
    "device_state_slots",
    "device_main_kv_pages",
    "device_backend_kv_pages",
    "host_state_slots",
    "host_kv_bytes",
};

using TokenId = std::int32_t;

inline constexpr std::uint32_t kMaximumConcurrency               = 8;
inline constexpr std::size_t kMaximumContextCacheSessionKeyBytes = 256;
// HOW MANY `cache_control` BREAKPOINTS A REQUEST MAY CARRY. Anthropic's API allows four, and this
// mirrored that -- which was right for a passthrough and wrong for this engine, whose agentic clients
// mark more than four blocks and got a 400 for a request the engine can serve perfectly well. Raised to
// 32 on 2026-10-01 as a DELIBERATE DIVERGENCE: nothing upstream enforces the limit here, and the number
// is a guard against pathological input rather than a compatibility rule. 32 is 8x the upstream limit,
// comfortably above the traffic that hit this, and still bounded.
//
// IT USED TO BE THE FLOOR ELSEWHERE TOO -- `max_shared_prefixes = max(concurrency, cap)` in
// `model_instance.cpp`, which is now decoupled to `kDefaultSharedPrefixFloor` below. The factor was never
// fixed: it depended on concurrency (8x at c=4, the shape this host runs).
inline constexpr std::size_t kMaximumExplicitPromptCacheMarkers  = 32;

// THE FLOOR `max_shared_prefixes` FALLS BACK TO when the caller sets none. It was accidentally the marker
// cap until 2026-10-01 (`max(concurrency, kMaximumExplicitPromptCacheMarkers)`), which meant raising that
// cap silently raised this default with it -- by 8x at the concurrency this host runs (c=4), not by a
// fixed factor, which is why a comment claiming "quadrupled" and another claiming "eight fold" were both
// wrong. Named here so the two uses cannot drift again.
inline constexpr std::uint32_t kDefaultSharedPrefixFloor = 4;
// Aggregate encoded image/video payload retained by one prompt, independent of item count.
inline constexpr std::size_t kMaximumPromptMediaBytes    = 256ULL << 20;
inline constexpr std::size_t kDefaultMediaCacheBytes     = 1ULL << 30;
inline constexpr std::size_t kDefaultMediaLiveBytes      = 2ULL << 30;
inline constexpr std::uint32_t kDefaultHostStateSlots    = 8;
// 8 GiB held back from pinned memory, as a FIRST CUT -- the number that justifies it is
// `~/ninfer-e2e/ab-runner.sh`'s gate, which treats MemAvailable >= 38 GiB as "memory released" on this
// 53 GB host, implying ~15 GiB is needed for weight staging, the media cache and the OS.
inline constexpr std::size_t kDefaultHostPinnedReserveBytes = 8ULL << 30U;
// One growth step. A state image is ~187 MiB, so 1 GiB lets several share a chunk while keeping the first
// pin modest.
inline constexpr std::size_t kDefaultHostPinnedChunkBytes   = 1ULL << 30U;

inline constexpr std::size_t kDefaultHostKvCapacityBytes = 8ULL << 30;

enum class KvCacheStorage : std::uint8_t {
    BFloat16,
    Int8Group64,
    Fp8E4M3Row256,
    Nvfp4Group16,
    Fp8KeyNvfp4Value,
};

enum class EnginePurpose : std::uint8_t {
    Generation,
    CausalScoring,
};

enum class KvCapacityMode : std::uint8_t {
    Explicit,
    Automatic,
};

inline constexpr std::size_t kDefaultKvCapacityHeadroomBytes = 1024ULL * 1024ULL * 1024ULL;

struct KvCapacityPolicy {
    KvCapacityMode mode                  = KvCapacityMode::Explicit;
    std::uint32_t explicit_tokens        = 2048;
    std::size_t automatic_headroom_bytes = 0;

    [[nodiscard]] static constexpr KvCapacityPolicy
    explicit_capacity(std::uint32_t tokens) noexcept {
        return KvCapacityPolicy{KvCapacityMode::Explicit, tokens, 0};
    }

    [[nodiscard]] static constexpr KvCapacityPolicy
    automatic(std::size_t headroom_bytes = kDefaultKvCapacityHeadroomBytes) noexcept {
        return KvCapacityPolicy{KvCapacityMode::Automatic, 0, headroom_bytes};
    }
};

enum class ProposalHead : std::uint8_t {
    Full,
    Optimized,
};

enum class SpeculativeBackend : std::uint8_t {
    None,
    Mtp,
    DFlash,
    DFlash2,
};

struct SpeculativeOptions {
    SpeculativeBackend backend = SpeculativeBackend::None;
    // Startup-fixed K: MTP 1..5; DFlash and DFlash2 1..15 (query width K+1).
    std::uint32_t draft_tokens = 0;
    ProposalHead proposal_head = ProposalHead::Full;
};

enum class StartupPhase : std::uint8_t {
    EngineStartup,
    CudaInitialize,
    ArtifactInspect,
    TargetPlan,
    WeightsMaterialize,
    WeightsStagingPin,
    TargetFinalize,
    FrontendInitialize,
    ProgramInitialize,
    HostStatePin,
    HostKvPin,
    CudaGraphPrepare,
    EngineFinalize,
};

enum class StartupStatus : std::uint8_t {
    Begin,
    Progress,
    Complete,
    Failed,
};

enum class StartupProgressUnit : std::uint8_t {
    None,
    Bytes,
};

struct StartupEvent {
    StartupPhase phase                = StartupPhase::EngineStartup;
    StartupStatus status              = StartupStatus::Begin;
    StartupProgressUnit progress_unit = StartupProgressUnit::None;
    std::uint64_t current             = 0;
    std::uint64_t total               = 0;
    std::uint64_t elapsed_ns          = 0;
};

struct StartupObserver {
    // Startup diagnostics never participate in Engine control flow. Callback exceptions are
    // ignored by the publishing boundary so a logging failure cannot invalidate model startup.
    std::function<void(const StartupEvent& event)> callback;
};

struct ContextCacheOptions {
    // Engine resolves every optional once at construction. With C=max_concurrency, the enabled
    // defaults are H=C, R=8, Host KV=8 GiB, P=2C, S=max(C,4) and L=2;
    // Engine::options() returns those effective values.
    bool enabled = true;
    // Extra Device checkpoint StateImage slots H. Total Device StateImage capacity is C + H.
    std::optional<std::uint32_t> device_state_slots;
    // THE ELASTIC PINNED BUDGET (2026-09-26). `host_state_slots` and `host_kv_capacity_bytes` are the
    // consumers' INITIAL SIZES: they draw on ONE shared pile of pinned host RAM that can grow past them,
    // bounded by the host's own RAM rather than by a flag. These three bound that growth.
    // **They are still the limits the planner plans against.** Growth is reachable only from execution
    // paths, and every demote option is priced against the capacity that exists NOW
    // (`admission_capacity()`), so a plan that passes feasibility already has room and nothing asks the
    // pool to grow. Setting a larger `--host-state-slots` is still how you plan for more host checkpoints;
    // the headroom is real and unspent, not a replacement for the flag.
    //   reserve : RAM pinned memory may never eat into. THE SAFETY PROPERTY -- see HostMemoryBudget.
    //   max     : 0 = no fixed ceiling; else a hard cap on the pile.
    //   chunk   : the size of one growth step.
    std::size_t host_pinned_reserve_bytes = kDefaultHostPinnedReserveBytes;
    std::size_t host_pinned_max_bytes     = 0U;
    std::size_t host_pinned_chunk_bytes   = kDefaultHostPinnedChunkBytes;
    // (Was: "Host StateImages and Host KV bytes are independently configured pinned-memory capacities.")
    std::uint32_t host_state_slots     = kDefaultHostStateSlots;
    // THE STATE AXIS'S CEILING (2026-10-05). Host KV and host state share one pinned pool, and the state
    // pool had no bound of its own: it grew until the pool was full, leaving the KV side no growth room, so
    // `Growable` could not be returned and the planner evicted restorable victims whose KV would have fitted.
    // ZERO MEANS UNBOUNDED -- today's behaviour -- so this changes nothing until it is set.
    // SET IT AT OR ABOVE `host_state_slots`, or the initial reservation itself is refused and the pool starts
    // smaller than configured. The right value is MEASURED: raising it costs KV room, lowering it forces
    // state evictions, and `host_state_growth_ceiling_refusals` is what makes that trade-off readable.
    std::uint32_t host_state_max_slots = 0;
    std::size_t host_kv_capacity_bytes = kDefaultHostKvCapacityBytes;
    // Bounded private/shared logical catalogs and per-continuation long-anchor count.
    std::optional<std::uint32_t> max_private_continuations;
    std::optional<std::uint32_t> max_shared_prefixes;
    std::optional<std::uint32_t> max_long_anchors_per_continuation;
};

struct ContextCostOptions {
    // Empty selects generic defaults plus any matching values compiled into the binary. A
    // nonempty runtime preset independently overrides its matching machine transfer and
    // artifact-prefill components; absent entries retain the preceding numerical layer.
    std::filesystem::path preset_path;
};

struct EngineOptions {
    std::filesystem::path artifact_path;
    std::filesystem::path chat_template_path;
    EnginePurpose purpose              = EnginePurpose::Generation;
    int device                         = 0;
    std::uint32_t max_context          = 2048; // Logical ceiling of one request or score window.
    KvCapacityPolicy kv_capacity       = KvCapacityPolicy::explicit_capacity(2048);
    std::uint32_t max_concurrency      = 1;
    std::uint32_t max_pending_requests = 16;
    std::uint32_t pending_timeout_ms   = 30000;
    std::uint32_t prefill_chunk        = 1024;
    KvCacheStorage kv_cache            = KvCacheStorage::BFloat16;
    SpeculativeOptions speculative;
    std::size_t media_cache_bytes = kDefaultMediaCacheBytes;
    std::size_t media_live_bytes  = kDefaultMediaLiveBytes;
    // Zero selects a bounded worker count from the detected host concurrency.
    std::uint32_t media_preprocess_threads = 0;
    bool enable_vision                     = false;
    // When true together with enable_vision, the Vision encoder runs entirely on host
    // (CPU) instead of the device. Vision weights stay in host DRAM and are not
    // materialized into the device arena, saving their VRAM cost.
    bool vision_cpu_offload                = false;
    bool use_cuda_graph                    = true;
    ContextCacheOptions context_cache;
    ContextCostOptions context_cost;
    StartupObserver startup_observer;
    float rope_scaling_factor              = 1.0f;
    std::uint32_t rope_scaling_original_context = 262144;
};

enum class SamplingMode : std::uint8_t {
    Thinking,
    NonThinking,
};

// Immutable model-owned values used when a request does not override a sampling field. Seed is
// deliberately excluded: it is an execution choice rather than a model recommendation.
struct SamplingPreset {
    float temperature       = 0.0F;
    std::int32_t top_k      = 0;
    float top_p             = 1.0F;
    float min_p             = 0.0F;
    float presence_penalty  = 0.0F;
    float frequency_penalty = 0.0F;
};

struct ModelSamplingDefaults {
    SamplingPreset thinking;
    SamplingPreset non_thinking;

    [[nodiscard]] constexpr const SamplingPreset& for_mode(SamplingMode mode) const noexcept {
        return mode == SamplingMode::Thinking ? thinking : non_thinking;
    }
};

// Public request-side overrides. std::nullopt means "use the registered model/mode default";
// explicit zero remains a real override (including temperature=0 for exact argmax).
struct SamplingOverrides {
    std::optional<float> temperature;
    std::optional<std::int32_t> top_k;
    std::optional<float> top_p;
    std::optional<float> min_p;
    std::optional<float> presence_penalty;
    std::optional<float> frequency_penalty;
    std::optional<std::uint64_t> seed;
};

// Complete parameters after Engine resolution. Target runtimes consume only this type.
struct ResolvedSamplingParameters {
    float temperature       = 0.0F;
    std::int32_t top_k      = 20;
    float top_p             = 1.0F;
    float min_p             = 0.0F;
    float presence_penalty  = 0.0F;
    float frequency_penalty = 0.0F;
    std::uint64_t seed      = 0;
};

enum class OutputChannel : std::uint8_t {
    Content,
    Reasoning,
};

struct StopString {
    std::string text;
    OutputChannel channel  = OutputChannel::Content;
    bool include_in_output = false;
};

struct StopPolicy {
    std::vector<TokenId> token_ids;
    std::vector<StopString> strings;
    bool include_model_defaults = true;
    bool publish_stop_token     = false;
};

struct ThinkingControlOptions {
    // Positive maximum accepted model-origin tokens while the Qwen thinking phase remains open.
    // Omitted means unlimited. Injected target-control tokens consume the total output budget but
    // not this model-origin budget.
    std::optional<std::uint32_t> budget;
};

struct ExecutionOptions {
    SamplingOverrides sampling;
    std::uint32_t requested_output_tokens = 0;
    bool allow_prefix_reuse               = true;
    ThinkingControlOptions thinking;
};

struct OutputOptions {
    bool raw                     = false;
    bool preserve_special_tokens = false;
    // Presentation constraint supplied by the protocol adapter. It bounds only Qwen's emitted
    // function-name grammar; it does not require the name to match a currently declared tool.
    std::uint32_t tool_name_max_length = 128;
    bool tolerant_tool_calls = false;
};

struct RequestOptions {
    ExecutionOptions execution;
    StopPolicy stop;
    OutputOptions output;
};

enum class MediaKind : std::uint8_t {
    Image,
    Video,
};

enum class ImageResizePolicy : std::uint8_t {
    Downsize,
    RejectOversized,
};

struct OwnedMedia {
    MediaKind kind = MediaKind::Image;
    std::vector<std::uint8_t> bytes;
    std::string media_type;
    std::string source_name;
    ImageResizePolicy image_resize_policy = ImageResizePolicy::Downsize;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

// Model-origin structured output. Protocol adapters own any wire-level call identifier.
struct GeneratedToolCall {
    std::string name;
    std::string arguments_json;
};

// Terminal interpretation of model-origin tool-call markup. Parameter schemas guide JSON
// normalization but do not validate the call; only a structure/identity failure can return a
// complete marker region to ordinary content.
enum class ToolCallParseFallbackReason : std::uint8_t {
    None,
    MalformedStructure,
    DuplicateParameter,
    InvalidToolName,
    UndeclaredTool,
    TrailingContent,
    // Tolerant recovery discarded a trailing suffix that followed an otherwise complete call.
    // A structured response was still produced, so this is surfaced for transparency rather than
    // treated as a fallback-to-text failure.
    TruncatedTail,
};

// #29'S DISCRIMINATOR, and it is a FIELD RATHER THAN A REASON VALUE ON PURPOSE. The values above describe
// the SYMPTOM, and the parser's own truncation on a literal close tag and a model that emitted bad markup
// produce the SAME ones -- so "is this a parser leak or the model's own output?" could not be answered from
// the log at all. Replacing the reason with the path would lose the symptom to gain the path; carrying both
// answers the question without destroying what was already known. Only ONE place in the tree can set this:
// `tool_call_parser.cpp`'s `find_parameter_close`, the arm that treats a close tag as payload content -- the
// arm whose own comment names the leak this fallback produces.
//
// IT REPORTS COEXISTENCE, NOT CAUSATION, and the distinction is deliberate: the recovery arm may have run in
// a value the parse then recovered past, leaving the fallback caused by something else. It means "the
// parser's recovery path was involved in this response", which is what #29 needs to know -- not "the parser
// caused it", which this cannot show.
enum class ToolCallParsePath : std::uint8_t {
    // Not established: no fallback occurred, or it occurred before the recovery arm could run.
    Unset,
    // The fallback was raised with the tolerant content-skip arm having already run on this response.
    AfterTolerantContentSkip,
};

[[nodiscard]] inline constexpr const char*
tool_call_parse_path_name(ToolCallParsePath path) noexcept {
    switch (path) {
    case ToolCallParsePath::Unset:
        return "unset";
    case ToolCallParsePath::AfterTolerantContentSkip:
        return "after_tolerant_content_skip";
    }
    return "unset";
}

[[nodiscard]] inline constexpr const char*
tool_call_parse_fallback_reason_name(ToolCallParseFallbackReason reason) noexcept {
    switch (reason) {
    case ToolCallParseFallbackReason::None:
        return "none";
    case ToolCallParseFallbackReason::MalformedStructure:
        return "malformed_structure";
    case ToolCallParseFallbackReason::DuplicateParameter:
        return "duplicate_parameter";
    case ToolCallParseFallbackReason::InvalidToolName:
        return "invalid_tool_name";
    case ToolCallParseFallbackReason::UndeclaredTool:
        return "undeclared_tool";
    case ToolCallParseFallbackReason::TrailingContent:
        return "trailing_content";
    case ToolCallParseFallbackReason::TruncatedTail:
        return "truncated_tail";
    }
    return "malformed_structure";
}

struct ToolCallParseDiagnostics {
    bool marker_seen                            = false;
    std::uint32_t structured_call_count         = 0;
    std::uint32_t empty_arguments_omitted       = 0;
    std::uint32_t schema_mismatch_arguments     = 0;
    ToolCallParseFallbackReason fallback_reason = ToolCallParseFallbackReason::None;
    // WHICH PATH RAISED THE FALLBACK (#29). See `ToolCallParsePath` for why this is a field beside the
    // reason rather than a reason value, and for what it does and does not claim.
    ToolCallParsePath parse_path = ToolCallParsePath::Unset;

    [[nodiscard]] friend constexpr bool
    operator==(const ToolCallParseDiagnostics&, const ToolCallParseDiagnostics&) noexcept = default;
};

// Wire-independent conversation authority. Protocol adapters preserve these roles and their
// ordering; a target frontend owns any model-specific role lowering.
enum class ChatRole : std::uint8_t {
    System,
    Developer,
    User,
    Assistant,
    Tool,
};

enum class MessagePartKind : std::uint8_t {
    Text,
    Media,
};

struct MessagePart {
    MessagePartKind kind = MessagePartKind::Text;
    std::string text;
    OwnedMedia media;
};

struct ChatMessage {
    ChatRole role = ChatRole::User;
    std::vector<MessagePart> parts;
    std::string reasoning_content;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
};

enum class ReasoningEffort : std::uint8_t {
    None,
    Minimal,
    Low,
    Medium,
    High,
    XHigh,
    Max,
};

[[nodiscard]] constexpr std::string_view reasoning_effort_name(ReasoningEffort effort) noexcept {
    switch (effort) {
    case ReasoningEffort::None:
        return "none";
    case ReasoningEffort::Minimal:
        return "minimal";
    case ReasoningEffort::Low:
        return "low";
    case ReasoningEffort::Medium:
        return "medium";
    case ReasoningEffort::High:
        return "high";
    case ReasoningEffort::XHigh:
        return "xhigh";
    case ReasoningEffort::Max:
        return "max";
    }
    return {};
}

enum class PromptContinuationMode : std::uint8_t {
    NewAssistantTurn,
    ContinueFinalAssistant,
};

struct PromptOptions {
    PromptContinuationMode continuation = PromptContinuationMode::NewAssistantTurn;
    std::optional<bool> enable_thinking;
    std::optional<ReasoningEffort> reasoning_effort;
    std::optional<bool> preserve_thinking;
    // JSON object of template parameters. Unset typed fields leave template defaults intact.
    std::string chat_template_kwargs_json;
    bool add_vision_id = false;
    std::vector<std::string> tool_jsons;
};

enum class CacheRetentionHint : std::uint8_t {
    Default,
    LiveSession,
    Disposable,
};

enum class PromptCacheMarkerKind : std::uint8_t {
    SharedStablePrefix,
    PrivateLongAnchor,
};

enum class SharedCandidateEvidence : std::uint8_t {
    None               = 0,
    ExplicitBoundary   = 1U << 0U,
    RequestedAutomatic = 1U << 1U,
    DefaultAutomatic   = 1U << 2U,
    EngineStructural   = 1U << 3U,
    EngineObserved     = 1U << 4U,
};

[[nodiscard]] constexpr SharedCandidateEvidence operator|(SharedCandidateEvidence left,
                                                          SharedCandidateEvidence right) noexcept {
    return static_cast<SharedCandidateEvidence>(static_cast<std::uint8_t>(left) |
                                                static_cast<std::uint8_t>(right));
}

constexpr SharedCandidateEvidence& operator|=(SharedCandidateEvidence& left,
                                              SharedCandidateEvidence right) noexcept {
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool has_shared_candidate_evidence(SharedCandidateEvidence value,
                                                           SharedCandidateEvidence evidence) {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(evidence)) != 0;
}

enum class PromptCacheMarkerLocation : std::uint8_t {
    MessageBoundary,
    MessagePartBoundary,
    LeadingInstructionBoundary,
    ToolBoundary,
};

struct PromptCacheMarker {
    std::uint32_t after_message_count  = 0;
    PromptCacheMarkerKind kind         = PromptCacheMarkerKind::SharedStablePrefix;
    SharedCandidateEvidence evidence   = SharedCandidateEvidence::ExplicitBoundary;
    PromptCacheMarkerLocation location = PromptCacheMarkerLocation::MessageBoundary;
    // Byte count within the untrimmed leading System/Developer message.
    std::uint32_t leading_instruction_bytes = 0;
    std::uint32_t after_tool_count          = 0;
    // For MessagePartBoundary, after_message_count identifies the containing message using a
    // one-based count and this value identifies the number of serialized parts within it.
    std::uint32_t after_message_part_count = 0;

    [[nodiscard]] friend constexpr bool operator==(PromptCacheMarker,
                                                   PromptCacheMarker) noexcept = default;
};

struct ContextCacheHints {
    std::optional<std::string> session_key;
    CacheRetentionHint retention = CacheRetentionHint::Default;
    std::vector<PromptCacheMarker> markers;
    // Protocols with their own automatic/explicit write policy disable the Engine's structural
    // candidates. Exact reads from already-published shared prefixes remain enabled.
    bool allow_engine_automatic_shared_prefixes = true;
    // Advance the named session lineage when session_key is present. This does not require an
    // anonymous content-matched source to be retained.
    bool update_session_index = true;
};

struct PromptInput {
    std::vector<ChatMessage> messages;
    PromptOptions options;
    ContextCacheHints context_cache;
};

enum class RequestErrorKind : std::uint8_t {
    ContextLengthExceeded,
    ThinkingBudgetCapacityInsufficient,
    MediaBudgetExceeded,
    InvalidMedia,
    Overloaded,
    QueueTimeout,
    Cancelled,
    Unavailable,
};

class RequestError final : public std::invalid_argument {
public:
    RequestError(RequestErrorKind kind, std::string message)
        : std::invalid_argument(std::move(message)), kind_(kind) {}

    [[nodiscard]] RequestErrorKind kind() const noexcept { return kind_; }

private:
    RequestErrorKind kind_;
};

struct PromptSummary {
    bool starts_in_reasoning    = false;
    std::uint32_t prompt_tokens = 0;
    bool has_media              = false;
};

struct PromptPreparationStats {
    double seconds                       = 0.0;
    double media_preprocess_seconds      = 0.0;
    double media_preprocess_work_seconds = 0.0;
    double tokenize_seconds              = 0.0;
    std::size_t media_items              = 0;
    std::size_t media_bytes              = 0;
    std::uint64_t raw_patches            = 0;
    std::uint64_t vision_tokens          = 0;
    std::size_t patch_bytes              = 0;
    std::size_t media_cache_hits         = 0;
    std::size_t media_cache_misses       = 0;
    std::size_t media_singleflight_waits = 0;
    std::size_t built_patch_bytes        = 0;
    std::size_t reused_patch_bytes       = 0;
};

struct MediaCacheSummary {
    std::size_t capacity_bytes       = 0;
    std::size_t live_capacity_bytes  = 0;
    std::size_t retained_bytes       = 0;
    std::size_t live_bytes           = 0;
    std::size_t entries              = 0;
    std::size_t inflight             = 0;
    std::size_t queued_tasks         = 0;
    std::size_t active_tasks         = 0;
    std::uint32_t preprocess_threads = 0;
    std::uint64_t hits               = 0;
    std::uint64_t misses             = 0;
    std::uint64_t singleflight_waits = 0;
    std::uint64_t evictions          = 0;
    std::uint64_t oversize_bypasses  = 0;
};

enum class FinishReason : std::uint8_t {
    None,
    OutputLimit,
    ContextCapacity,
    StopToken,
    StopString,
    Cancelled,
};

struct OutputDelta {
    OutputChannel channel = OutputChannel::Content;
    std::string text;
};

// Exact prompt accounting selected at admission. Streaming consumers receive this once before any
// OutputDelta, after the prefix choice and materialization reservation are committed and before
// transfer/prefill execution.
struct GenerationStart {
    PromptSummary prompt;
    std::uint32_t reused_prompt_tokens = 0;
};

// Cumulative prompt frontier published only after the corresponding Program work has completed.
// The Engine owns the request-relative clock and accounting; protocol adapters choose names and
// units for the wire representation.
struct PromptProgress {
    std::uint32_t total_prompt_tokens     = 0;
    std::uint32_t reused_prompt_tokens    = 0;
    std::uint32_t processed_prompt_tokens = 0;
    std::uint64_t elapsed_ns              = 0;
};

// Cumulative timing snapshot at one stable output-commit boundary. Generated tokens count model
// and Engine-injected tokens accepted into the sequence, independently of whether the Frontend has
// enough visible bytes to publish an OutputDelta for that boundary.
struct GenerationTimingObservation {
    std::uint32_t generated_tokens      = 0;
    std::uint64_t prompt_elapsed_ns     = 0;
    std::uint64_t generation_elapsed_ns = 0;
};

class OutputSink {
public:
    virtual ~OutputSink()                                   = default;
    virtual void start(GenerationStart start)               = 0;
    virtual void progress(PromptProgress progress)          = 0;
    virtual void timing(GenerationTimingObservation timing) = 0;
    virtual void publish(OutputDelta delta)                 = 0;
};

enum class OutputConsumerMode : std::uint8_t {
    Aggregate,
    Streaming,
};

// Observation affects only request publication. It never changes model execution, output
// semantics, scheduling, or cache selection. Live observations require a Streaming consumer;
// phase timings may also be retained for an Aggregate terminal response.
struct GenerationObservationOptions {
    bool phase_timings   = false;
    bool live_timings    = false;
    bool prompt_progress = false;
};

class CancellationView {
public:
    CancellationView() = default;
    explicit CancellationView(std::function<bool()> requested);

    [[nodiscard]] bool requested() const;

private:
    std::function<bool()> requested_;
};

// Deadline and cancellation apply to all host-side prompt preparation work. Empty values mean
// unbounded preparation.
struct PreparationControl {
    std::chrono::steady_clock::time_point deadline;
    CancellationView cancellation;
};

// Request-stage wall timings retained for end-to-end latency/rate reporting. Prefill/decode are
// Program execution elapsed time and include Device completion waits; total also includes queueing
// and other request lifetime. They are not Host-work phases. GenerationEngineTiming below is the
// direct, mutually-exclusive Host observation contract.
struct GenerationTimings {
    double prepare_seconds     = 0.0;
    double first_token_seconds = 0.0;
    double vision_seconds      = 0.0;
    double prefill_seconds     = 0.0;
    double decode_seconds      = 0.0;
    // Request wall phases at committed model-state boundaries. Prompt begins when admission and
    // its exact reuse choice are published and ends at the first accepted output token. Generation
    // spans the first through last accepted output token and therefore has N-1 token intervals.
    double prompt_wall_seconds     = 0.0;
    double generation_wall_seconds = 0.0;
    double total_seconds           = 0.0;
};

// Wall elapsed time directly observed in Engine-owned regions. "Exposed" values are latency
// exposure: every active request delayed by one compact-batch unit observes that unit's full
// elapsed time, so values from concurrent requests must not be summed. Device wait is reported
// separately from Host-active work.
struct GenerationEngineTiming {
    double queue_wait_seconds                   = 0.0;
    double engine_boundary_exposed_seconds      = 0.0;
    double program_submit_exposed_seconds       = 0.0;
    double program_post_exposed_seconds         = 0.0;
    double engine_commit_output_exposed_seconds = 0.0;
    double engine_maintenance_exposed_seconds   = 0.0;
    double device_wait_exposed_seconds          = 0.0;
    double decode_host_exposed_seconds          = 0.0;
    double decode_device_wait_exposed_seconds   = 0.0;
    std::uint64_t prefill_units                 = 0;
    std::uint64_t decode_rounds                 = 0;
    std::uint64_t control_units                 = 0;
    // WHAT THIS REQUEST'S MATERIALIZATION MOVED, by direction -- the ONLY record of the restore it paid.
    // Measured 2026-10-01: `proc` minus the recorded `prefill` is ordered by reuse path exactly as restore
    // volume should be (`private_long_anchor` 1.82s on 5,609 tokens to prefill, `private_response_replay`
    // 0.06s -- the path that restores nothing), and req#50 spent 6.07s of proc with 0.59s of prefill that
    // nothing in its record could account for. Summed from `ContextTransferObservation::elapsed_ns`, which
    // already existed per resource and direction on the materialization outcome but never reached the
    // request.
    double restore_seconds                      = 0.0;  // HostToDevice: resuming from a checkpoint
    double demote_seconds                       = 0.0;  // DeviceToHost: parking one
    // KV pages only.  The State transfer site passes page_count=0 by construction
    // (`materialization.cpp` `state_restore`), so a request that restored a state image with no KV
    // shows a non-zero `restore_seconds` against `restore_pages == 0` -- that pair is the state-only
    // restore, not a missing sample.  Do not read `pages` as a denominator for `seconds`.
    std::uint64_t restore_pages                 = 0;
    std::uint64_t demote_pages                  = 0;
    // THE SAME TWO TERMS, SCOPED TO THE FIRST-TOKEN WINDOW. `device_wait_exposed_seconds` and the host
    // phases above run for the WHOLE request, but `ttft` -- and therefore `proc = ttft - queue`, the number
    // everyone reads -- stops at the first token. Subtracting one from the other mixes two windows, which is
    // why `ttft - queue - host_total - prefill` could not be read as a residual: on 290 measured records its
    // median was -7 ms while 33 records carried a median 3.5 s, and the host term in it was whole-request.
    // These are a COPY of the accumulators taken where `first_token` is SET (`record_committed_output`), so
    // `ttft - queue - ttft_host_exposed_seconds - ttft_device_wait_seconds` is a residual over ONE window.
    // (It subtracted `prefill_seconds` first, which is program WALL time already containing the same
    // submit/post host time the host term contains -- so the host term was double-counted and the value
    // went negative on real traffic. See `request_log.cpp`.)
    // **THE PLACEMENT IS THE WHOLE CORRECTNESS ARGUMENT, AND THE FIRST VERSION GOT IT WRONG.** It froze in
    // `finish_program_call`, claiming the commit happens inside the program call. It does not:
    // `program_call.finish()` inside `commit_pending` (and the forced-token path's own) runs BEFORE
    // `record_committed_output`, so that freeze fired a round late. Here
    // is right because the round's `device_wait_ns` is already accumulated by that `program_call.finish()`
    // and nothing between
    // adds any -- and the residual's own CHECK is that it must be NON-NEGATIVE (`ttft - queue - host -
    // device_wait`). **A LATE freeze shows as a NEGATIVE value, so that sign DOES catch over-inclusion;
    // what it does NOT catch is the opposite error** -- anything the window misses inflates the value,
    // so an early or incomplete window still reads non-negative. (An earlier version of this comment
    // said the check "cannot catch a misplacement", which was wrong: it catches a late one, and the
    // first version of the code shipped a residual that was negative in 78 of 290 records because the
    // subtrahend was the wrong term.)
    double ttft_host_exposed_seconds             = 0.0;
    double ttft_device_wait_seconds              = 0.0;
    bool ttft_window_frozen                      = false;
};

struct SpeculativeStats {
    SpeculativeBackend backend    = SpeculativeBackend::None;
    bool enabled                  = false;
    std::uint32_t draft_window    = 0;
    std::uint64_t rounds          = 0;
    std::uint64_t drafted_tokens  = 0;
    std::uint64_t accepted_tokens = 0;
    std::uint64_t fallback_steps  = 0;
    std::vector<std::uint64_t> accepted_per_position;
};

struct ThinkingBudgetStats {
    std::optional<std::uint32_t> configured_budget;
    // Model-origin tokens accepted while capped thinking remained open.
    std::uint32_t model_thinking_tokens = 0;
    // Complete tokenizer-derived target-control suffix committed by Engine.
    std::uint32_t injected_tokens = 0;
    bool applied                  = false;
};

enum class PrefixReusePath : std::uint8_t {
    Root,
    PrivateEndpoint,
    PrivateTurnClosure,
    PrivateResponseReplay,
    PrivateLongAnchor,
    SharedStablePrefix,
};

// Why bounded pressure planning stopped for the materialization decision committed to one request.
enum class MaterializationStopReason : std::uint8_t {
    NoPressure,
    QueueExhausted,
    TargetBudget,
    ExpansionCapacity,
    TimeBudget,
    InsufficientExpectedGain,
    WorkBudget,
};

[[nodiscard]] inline constexpr const char*
materialization_stop_reason_name(MaterializationStopReason reason) noexcept {
    switch (reason) {
    case MaterializationStopReason::NoPressure:
        return "no_pressure";
    case MaterializationStopReason::QueueExhausted:
        return "queue_exhausted";
    case MaterializationStopReason::TargetBudget:
        return "target_budget";
    case MaterializationStopReason::ExpansionCapacity:
        return "expansion_capacity";
    case MaterializationStopReason::TimeBudget:
        return "time_budget";
    case MaterializationStopReason::InsufficientExpectedGain:
        return "insufficient_expected_gain";
    case MaterializationStopReason::WorkBudget:
        return "work_budget";
    }
    return "no_pressure";
}

enum class MaterializationSearchPhase : std::uint8_t {
    None,
    Setup,
    Construction,
    Assessment,
    Expansion,
    Refinement,
};

[[nodiscard]] inline constexpr const char*
materialization_search_phase_name(MaterializationSearchPhase phase) noexcept {
    switch (phase) {
    case MaterializationSearchPhase::None:
        return "none";
    case MaterializationSearchPhase::Setup:
        return "setup";
    case MaterializationSearchPhase::Construction:
        return "construction";
    case MaterializationSearchPhase::Assessment:
        return "assessment";
    case MaterializationSearchPhase::Expansion:
        return "expansion";
    case MaterializationSearchPhase::Refinement:
        return "refinement";
    }
    return "none";
}

struct MaterializationDiagnostics {
    std::uint64_t predicted_now_ns           = 0;
    std::uint64_t predicted_future_loss_ns   = 0;
    std::uint64_t predicted_total_ns         = 0;
    std::uint32_t targets_evaluated          = 0;
    std::uint64_t projection_work            = 0;
    std::uint64_t planning_elapsed_ns        = 0;
    std::uint64_t search_elapsed_ns          = 0;
    MaterializationStopReason stop_reason    = MaterializationStopReason::NoPressure;
    bool budget_exhausted                    = false;
    // THE SEARCH STOPPED BECAUSE THE TARGET ARENA WAS FULL -- a CAPACITY ceiling, which `budget_exhausted`
    // does not distinguish from the designed stop (the search spending its own budget). Measured
    // 2026-09-28: the arena filled on the e2e's phase 1 and the request died as an HTTP 500 plus a worker
    // recovery, on every tree including the pre-change control, which is what made phase 1 unreachable.
    // Now that condition truncates the search instead, and this flag is what keeps the truncation visible.
    bool target_arena_truncated              = false;
    std::uint32_t selected_degradation_units = 0;
    bool selected_maximal_fallback           = false;
    // #6 (2026-09-27): A PAIR, NOT A VERDICT ON ITS OWN, and each half alone is ambiguous.
    //   `feasible_preserving_alternatives` -- how many targets the search assessed FEASIBLE whose cost
    //      evicted no restorable victim. >0 means a plan that would have preserved one was available.
    //   `chosen_restorable_evictions` -- the ADOPTED plan's count of evictions of victims that held a
    //      recoverable checkpoint (FoldedCost::restorable_evictions, the field the key now ranks first).
    // Both >0 in the same record is #6 surviving: a preserving plan existed and a destroying one was taken.
    // `chosen_restorable_evictions > 0` with the other 0 is the honest other case -- nothing feasible
    // preserved, which is what the three post-fix prod evictions looked like (host KV 94% full).
    std::uint64_t feasible_preserving_alternatives = 0;
    // THE UNGATED DENOMINATOR FOR THAT COUNTER, and the reason it is needed: `feasible_preserving_alternatives`
    // is gated on a logical goal, so it reads 0 both when a preserving target was assessed and found
    // goal-less AND when no preserving target was assessed AT ALL -- and only the second is #6's shape (a
    // restorable checkpoint destroyed while a demote was available). Read them together: `assessed == 0`
    // is "the planner never had a preserving option in front of it"; `assessed > 0 && feasible == 0` is
    // "preserving options existed and none was adoptable", which is a defensible outcome.
    std::uint64_t preserving_alternatives_assessed = 0;
    // WHERE A PLANNING RUN'S GOAL PROBES CAME FROM, one entry per call site in the planner. Measured
    // 2026-10-01: 67% of plans stop on `time_budget` at ~4,630 probes and 82% stop on some budget, plans
    // with a SINGLE candidate included -- so the cost is a fixed per-request bill and this says which site
    // is repeating. Order is the planner's `kGoalProbeSiteNames`.
    std::array<std::uint64_t, 5> goal_probes_by_site{};
    // The selection the search settled on, for scale: `candidates` alone does not say how many targets the
    // search visited before stopping.
    std::uint32_t targets_assessed = 0;
    std::uint64_t chosen_restorable_evictions      = 0;
    // THE SCOPE, AND A CORRECTION. This counts victims from the plan's whole assessment -- private AND
    // shared owners -- while `/stats`' `private_owners_evicted`, `private_evictions_demotable`,
    // `private_eviction_checks` and the `private victim evicted:` journal line are all PRIVATE-ONLY (that
    // print loops over `transaction.victim_count`). The shared half has no journal line at all, so a shared
    // eviction is visible only through `/stats`.
    // **BUT DO NOT read `pressure.owners_evicted_total` as "what this counted", which an earlier version of
    // this comment claimed.** They are different populations: this one is the chosen PLAN's predicted
    // evictions of victims holding a recoverable checkpoint; that one is owners COMMITTED as `Evicted`,
    // restorable or not. The relation is plan-versus-committed, and the repo already records that the plan
    // count UNDERCOUNTS the committed set. The original inference rested on `13 + 5 = 18` in one request; a
    // review found a second coincidence (60 and 60) and killed it. Two coincidences are not a mechanism.
    // AND WHY THE PRESERVING TARGET WAS NOT COUNTED (2026-09-27). A code analysis showed that a
    // zero-eviction target can fail for a reason neither `demotable` nor `demote_possible` can see: a plan
    // obtains its publication cell from its own consumed private source, a VACANT catalog cell, or a victim
    // it EVICTS (`resource_manager.h:2130-2191`) -- so a fully-preserving target has no cell and is
    // unadoptable, and the pair above cannot count it because its counter sits behind the same `goal`.
    // This counts the targets that were assessed and produced NO goal, which is the difference between
    // "the relief step was generated and could not be adopted" and "it was never reached".
    std::uint64_t assessed_targets_without_goal       = 0;
    // WHY THOSE TARGETS HAD NO GOAL -- the split that makes the line above interpretable, and it was needed
    // before the line could be read at all. `logical_goal` is called only for a PHYSICALLY FEASIBLE
    // assessment (`logical_goal` is called only for a FEASIBLE one), so a target that is not feasible arrives at the
    // no-goal increment having NEVER HAD A PROBE TO FAIL. The single total therefore mixes two populations
    // that mean opposite things, and on 2026-10-01 that cost a wrong conclusion: "~4000 of ~4080 targets had
    // no goal" was read as "the goal gate refused them" -- while the goal-probe reason split measured
    // **0 refusals against 417,152 probes** on live QA. The cell refused nothing; the population was
    // INFEASIBILITY, which is a capacity fact, not a catalog one.
    //   `infeasible`   -- physical_status != Feasible: no probe was made, the target could not be built
    //                     (capacity / room / placement).
    //   `unadoptable`  -- Feasible, probe made, and `logical_goal` returned nullopt (the goal gate: no
    //                     publication cell obtainable, or one of the earlier rejections in the probe).
    // `infeasible + unadoptable` is exactly `assessed_targets_without_goal`.
    std::uint64_t assessed_targets_without_goal_infeasible  = 0;
    std::uint64_t assessed_targets_without_goal_unadoptable = 0;
    // PER-REQUEST CANDIDATE VISIBILITY (2026-09-27), added because a served run showed twelve consecutive
    // requests reusing exactly 23,353 tokens through `shared_stable_prefix` while their prompts ran 39k-56k,
    // and nothing in the record could say whether a LONGER source had existed and been refused, or whether
    // the frozen snapshot was simply the longest thing available. The journal's `reuse-select` line answers
    // that in principle, but it fires for the first 8 selections per process, so it was silent for exactly
    // the requests being asked about; the reason a candidate had no goal was folded into the conflating
    // counter above. This is the same information ATTACHED TO THE REQUEST.
    //
    // One entry per candidate the planner built, in the order it built them.
    struct MaterializationCandidate {
        std::uint32_t reuse      = 0;  // tokens this candidate would have reused (its own plan summary)
        std::uint32_t probes     = 0;  // goal-builder probes the planner made FOR it
        std::uint32_t goals      = 0;  // probes that produced an adoptable goal
        std::uint32_t cell_only  = 0;  // probes blocked by the publication cell and by NOTHING else
        std::uint32_t other      = 0;  // probes blocked for any other reason
        bool          winner     = false;
        bool          private_source = false;
        bool          shared_source  = false;
        // WAS IT ELECTION-ELIGIBLE, and what did the election rank it on? `reuse` alone cannot answer the
        // question this row exists for, because `FoldedCost::key()` ranks TEN terms ABOVE
        // `reused_prompt_tokens` (see `materialization_budget.h`): a candidate with more reuse losing is the
        // ORDINARY case whenever any of them differs, so `longer_lost` as a bare boolean asserts a defect the
        // ordering may have applied on purpose. These are that ordering's leading terms, so a loss can be
        // attributed to the element that decided it instead of to reuse.
        // `eligible` = the planner reached a PHYSICALLY FEASIBLE assessment for it and the goal builder
        // produced a goal (the same condition `becomes_incumbent` uses); an ineligible candidate was never
        // compared at all, and counting it as a loser is what made this flag unreadable.
        bool          eligible               = false;
        std::uint32_t restorable_evictions   = 0;  // key element 0 -- the operator's #6 ruling
        std::uint64_t total_ns               = 0;  // key element 1
        std::uint64_t affected_selected_hits = 0;  // key element 2
        std::uint32_t owner_evictions        = 0;  // key element 4
        std::uint32_t checkpoint_drops       = 0;  // key element 5
        // See `ElectionTerm`: the winner's row is its SEATED cost, so a re-derivation of the election must
        // read these instead. `elected_*` are meaningful only where `seated_differs` is true (elsewhere the
        // row's own fields already ARE the elected ones).
        bool          seated_differs                = false;
        std::uint32_t elected_reuse                 = 0;
        std::uint32_t elected_restorable_evictions  = 0;
        std::uint64_t elected_total_ns              = 0;
        // Elements 2 and 4 as well, so a re-derivation can compare ALL the terms above reuse rather than
        // the three of them the first version exported -- comparing three let a loser that TIES on 0 and 1
        // and is worse on element 4 read as a domination, which is a legitimate election.
        std::uint64_t elected_affected_selected_hits= 0;
        std::uint32_t elected_owner_evictions       = 0;
        std::uint32_t elected_checkpoint_drops      = 0;
    };
    std::vector<MaterializationCandidate> candidates;
    // WHICH TERM OF THE ORDERING DECIDED THE WINNER OVER THE BEST LOSER. An index into `FoldedCost::key()`
    // (0 = `restorable_evictions` ... 10 = `reused_prompt_tokens`, 11 = `current_session_binding`,
    // 12 = `candidate_ordinal`, 13 = `target_ordinal`), or 255 when there was no eligible loser
    // to compare against (the honest value for "no longer candidate existed", which a bare
    // `longer_lost=false` cannot distinguish from "one existed and was ineligible").
    // Its purpose: `longer_lost` fires on ANY of the ten terms ranked above reuse, so the interesting
    // question is never "did a longer candidate lose" but "WHY". Element 0 means the #6 preservation
    // ruling decided (by design); the ORDINAL elements (12/13) mean the winner was picked by ENUMERATION
    // ORDER on a full tie, which is arbitrary and is the shape that would be a real bug.
    //
    // **DO NOT READ 10 AS "reuse itself decided, the real defect".** An earlier legend said exactly that,
    // and it is impossible to observe: element 10 is `max - reused_prompt_tokens`, so a cost with MORE
    // reuse has a SMALLER key and wins on element 10. A loser holding more reuse therefore lost on one of
    // elements 0..9 and `first_differing_key_element` cannot return 10 for that pair. **10 can only appear
    // beside a loser with LESS reuse** -- in which case there is no "longer candidate lost" to explain.
    // So (`longer_lost_eligible` && `decided_by == 10`) means the two costs are not the ones the election compared:
    // an instrument fault, and the test asserts the invariant so it cannot be read as a finding.
    // **AND `decided_by` IS COMPUTED EVEN WHEN `longer_lost_eligible` IS FALSE**, where it describes the winner's
    // margin over the best eligible loser rather than the loss of a longer candidate -- read it only
    // together with the two reuse fields, never alone.
    // 254 (`kNotAnElectedWinner`) means the recorded winner does NOT beat the loser on `key()`, so no
    // element can be named -- an instrument fault (a cost the election never ranked), not a finding.
    // 255 means NO ELIGIBLE LOSER EXISTED AT ALL, and nothing else -- an eligible loser that reuses nothing
    // still produces an attribution, so this is stronger than the first version, where 255 also covered
    // "eligible losers existed, none reusing anything".
    std::uint8_t longer_lost_decided_by = 255;
    // The same comparison restricted to ELIGIBLE candidates -- the one that can be called a defect.
    // **TWO DIFFERENT FOLDS, AND THIS IS THE PAIR THAT DIFFERS FROM `longer_lost`.** `longer_lost` /
    // `best_loser_reuse` / `chosen_reuse` are computed in `resource_manager` from `row.reuse`, which is the
    // plan's `summary().reusable_prompt_tokens`; THIS pair comes from the planner's election costs, whose
    // reuse is `machine_work.reused_prompt_tokens` (`fold_identity` / `fold_assessment`). The two agree on
    // live records so far, but they are not the same quantity by construction, so do not treat a
    // disagreement between `best_loser_reuse` and `best_eligible_loser_reuse` as an error in either one.
    std::uint32_t best_eligible_loser_reuse = 0;
    bool          longer_lost_eligible      = false;
    // PER-CANDIDATE ELECTION TERMS, index-aligned with `candidates` above. Filled by the PLANNER, which is
    // the only place the election's own costs exist (the planner folds a cost per candidate as it assesses
    // it); `resource_manager` merges these into the rows. Kept as a separate transport rather than folded
    // into the row so the row can stay a serialization type.
    struct ElectionTerm {
        bool          eligible               = false;  // feasible assessment AND a goal: eligible to win
        std::uint32_t restorable_evictions   = 0;
        std::uint64_t total_ns               = 0;
        std::uint64_t affected_selected_hits = 0;
        std::uint32_t owner_evictions        = 0;
        std::uint32_t checkpoint_drops       = 0;

        // THE WINNER'S COST AS THE ELECTION RANKED IT, BESIDE THE ONE THAT RAN. Every field above is
        // `elected_cost` for a LOSER and `seated_cost` for the WINNER -- deliberately, so the winner's row
        // describes the plan that actually ran. That is right for reading the outcome and WRONG for
        // re-deriving the election: a seal that changes the plan makes the winner's row worse than the plan
        // the election chose, so a re-derivation can "find" a candidate that dominates a winner the
        // election never preferred. `seated_differs` says the two disagree; the four values are the
        // elected ones and are the ONLY terms a re-derivation may compare. Meaningful on the winner's term.
        bool          seated_differs                = false;
        std::uint32_t elected_reuse                 = 0;
        std::uint32_t elected_restorable_evictions  = 0;
        std::uint64_t elected_total_ns              = 0;
        // Elements 2 and 4 as well, so a re-derivation can compare ALL the terms above reuse rather than
        // the three of them the first version exported -- comparing three let a loser that TIES on 0 and 1
        // and is worse on element 4 read as a domination, which is a legitimate election.
        std::uint64_t elected_affected_selected_hits= 0;
        std::uint32_t elected_owner_evictions       = 0;
        std::uint32_t elected_checkpoint_drops      = 0;
    };
    std::vector<ElectionTerm> election;
    // (The attribution itself is computed IN THE PLANNER, from the election's own `FoldedCost`s, by
    // `first_differing_key_element` in `materialization_budget.h`. It used to live here as a hand-carried
    // list of six terms over the ROW fields, and a review showed that version reported "element 10" for
    // pairs decided by any of the five elements it did not carry -- 10 being the value its legend called
    // "the real defect". It also read the rows, which on the search path hold the LAST assessment for a
    // candidate rather than the cost it was elected on. Both faults are removed by computing in the
    // planner from `FoldedCost`; do not reintroduce a row-based attribution here.)
    // The selection comparison itself, promoted out of the rate-limited journal line: `longer_lost` true
    // means a candidate reusing strictly MORE than the adopted plan was present in the candidate set and
    // lost. CORRECTED 2026-10-02 -- the original text here called that "the defect this pair exists to
    // catch", and THAT WAS WRONG: `reused_prompt_tokens` is element 10 of `FoldedCost::key()`, so a
    // higher-reuse candidate losing is the ordinary case whenever any of the ten terms ranked above it
    // differ, and a live reading showed the flag firing at 3.3% of requests while naming no cause. Read
    // `longer_lost_eligible` (eligible candidates only) for the defect-shaped comparison, and
    // `longer_lost_decided_by` for the term that decided it.
    std::uint32_t chosen_reuse     = 0;
    std::uint32_t best_loser_reuse = 0;
    bool          longer_lost      = false;
    // THE SPLIT (plan.md §2f), per request: the deepest TOKEN-EXACT common prefix between this prompt and any
    // stored ledger, the deepest RESTORABLE checkpoint at or below it, and how many entries were scanned (the
    // denominator -- without it a zero cannot be told from "nothing was examined"). Its verdict is
    // pre-committed in §2f, so this is not a counter to interpret later: best_tokens ~23k means the PROMPT
    // diverged there and no engine in the surveyed set would do better; best_tokens deep (say 60k) with
    // best_restorable shallow (23k) means the match exists and our retention or placement lost it, which is
    // ours. Scanning the CATALOG directly rather than the pressure-enumerated owner list is deliberate: that
    // list is built lazily and its emptiness has already been mistaken for an empty catalog once.
    std::uint32_t split_best_tokens     = 0;
    std::uint32_t split_best_restorable = 0;
    std::uint32_t split_entries         = 0;
    // DOES THE TOKENS-MATCH-BUT-RENDER-DIFFERS CASE EXIST? `split_best_tokens` says how far the prompt matches
    // a stored ledger token-by-token; the identity chain is a STRICTER test (the same tokens, rendered the
    // same). When the tokens match deeply and the identity does not, the stored entry was built from a
    // differently-RENDERED history -- which is the shape a re-rendered or thinking-stripped earlier turn
    // produces, and it is the one thing the token comparison cannot distinguish. This is the half the first
    // version of the split deliberately left OUT because the prompt was not reachable at that layer; it is
    // reachable now, and the V2->v3 audit's group A (40 of the 55 ceiling requests) is exactly this question.
    bool          split_identity_ok     = false;
    // 0 = the deepest match stopped because the tokens differ; 1 = the stored ledger ended first; 2 = the prompt
    // ended first (the ledger is a PREFIX of the prompt). Read it with `split_best_tokens`: the same number means
    // opposite things depending on this.
    std::uint8_t  split_ended_by        = 0;
    // WHERE THE DIVERGENCE IS, which `split_ended_by = 0` alone cannot say. `split_best_tokens` is the match
    // and this is the deepest entry's OWN ledger length, so the three numbers read together localise the stop:
    // a match short of BOTH means the tokens really differ at that index, and `split_best_tokens` against the
    // prompt's own length says whether that index falls inside this turn or past the previous one's end. The
    // 2026-09-27 finding ("divergence at the replayed-assistant boundary") was inferred from the match alone
    // and stayed an inference for exactly this missing denominator.
    std::uint32_t split_best_stored     = 0;
    // WHY the conversation's session cell was not a candidate, which `session_cell_offered` states but cannot
    // explain: on its first real reading that flag was FALSE in 100% of shared-class requests, including ones
    // whose cell held a frontier at 39k/42k/51k tokens. Read WITH `session_cell_frontier`, which says whether
    // there was a cell at all -- a reason here is only meaningful when the frontier is non-zero.
    // PRIVATE ENTRIES ONLY: a shared catalog slot is a separate index namespace and must never write here.
    // 0 = no cell / no session key / `update_session_index` false / cache disabled; 1 = offered (and STICKY);
    // 2 = the slot has no index entry at all; 3 = no shortlist key at its frontier; 4 = identity-tag mismatch;
    // 5 = digest mismatch; 7 = the entry is held by a running lane's edge; 8 = `inspect_admission` refused it.
    // COVERAGE, because a value no test can produce is a value a reader may be inventing: 1, 3, 4, 5 and 8 are
    // pinned by value in `test_resource_manager`; 9 only as an absence (never emitted on the no-cell path);
    // 7 is UNTESTED -- it needs a second lane, and a mutant deleting that site survives. 2 is the provisional
    // "no index entry at all".
    // 6 is retired as unreachable. 9 ("failed validation") is emitted at that site but is unreachable on the
    // path audited so far: the index is validated with the same criteria immediately before the loop and
    // nothing mutates the catalog in between; a slot recycled after the rebuild would land there and that case
    // is NOT audited (see `resource_manager.h`). 0 was ALSO unreachable for a while -- with no session cell, the no-cell
    // sentinel equalled the unoccupied entry's slot sentinel and every such request wrote 9, so the default
    // path (the Bash classifier's traffic) read "failed validation".
    std::uint8_t  session_cell_skip     = 0;
    // THE CONVERSATION'S OWN ENDPOINT, separately: one catalog slot holds several index entries (endpoint,
    // rewrite, each long anchor), so once a shallower anchor has been offered `session_cell_skip` is a sticky 1
    // and can no longer say why the endpoint was lost -- which is the question. Same codes; 0 = the loop never
    // met an entry that is this slot's endpoint.
    std::uint8_t  session_endpoint_skip = 0;
    // WHERE THE DEEPEST MATCH STOPPED, as an index: equal to `split_best_tokens` when `split_ended_by` is 0,
    // and 0 otherwise, so it is meaningful only beside that discriminator. Unconditional -- the id windows it
    // once accompanied were removed 2026-09-28 (they had answered, and they logged content).
    // WHICH LEDGER the deepest match came from -- 1 = the session cell THIS request's key resolves to,
    // 2 = any other private entry, 3 = shared, 0 = no entry matched any token. The divergence is only
    // interpretable beside this: the deepest match across the catalog is frequently another conversation's
    // ledger, whose divergence says nothing about why this conversation's own entry was refused.
    // 2 includes this conversation's SUPERSEDED ledgers: replacement drops their session marker, so they are
    // no longer its cell. `split_best_frontier` is that entry's own restorable point at or below its match --
    // NOT a resume point, and not comparable to `session_cell_frontier`.
    std::uint8_t  split_best_source     = 0;
    std::uint32_t split_best_frontier   = 0;
    std::uint32_t split_probe_index     = 0;
    // WHICH TURN OF THE PROMPT'S OWN HISTORY the divergence fell in, and how far into it the match
    // got. `split_probe_index` names a token and a token index is unreadable on its own -- nothing
    // a reader can consult retains the prompt's per-message layout -- so this is what makes the stop
    // legible: "8 tokens into message 3, a user turn" rather than "23,401".
    //
    // PROMPT-SIDE ONLY, from the deepest-matching entry (`split_best_source` says which ledger that
    // was). The STORED ledger's boundaries are not retained, so this does not say where the stored
    // render sat, and it must not be read as a delta between the two renders.
    // Meaningful only when `split_ended_by == 0`: an entry whose ledger or prompt simply ran out did
    // not diverge, and its fields keep their defaults.
    std::uint32_t split_message_index   = 0;
    std::uint32_t split_message_offset  = 0;
    // The role of that message (a ChatRole value), or 255 when no message starts at that index --
    // the end-of-prompt boundary, which starts none. 255 is not a role and must not be read as one.
    std::uint8_t  split_message_role    = 0xFF;
    bool          split_past_last_message = false;
    // THE SIBLING CONDITION AND THE RETAIN DECISION, counted even while the behaviour is off (2026-09-27).
    // `sibling_candidates` is how often a private source was found whose own endpoint lies BEYOND this
    // request's prompt -- a request that cannot reach the endpoint it is about to consume -- and
    // `retained_sources` / `consumed_sources` say what the planner did with the sources it did offer. The
    // audit's chain asserts such a request takes `Replace` and destroys the conversation's endpoint; these
    // three numbers size the population BEFORE anything changes behaviour, and verify the chain from traffic.
    // WHAT THE SESSION CELL HOLDS, versus what was offered. The conversation's session cell holds ONE entry —
    // the newest continuation published for it — and this reports that entry's frontier and whether it became
    // a candidate at all. Without it, "the newest fork is not the one we resume from" cannot be told from "the
    // newest fork was never offered", and those have opposite fixes. Measured case that motivated it: a
    // conversation whose 71,655 fork was used at one request, and whose next request at a 109,798-token prompt
    // was offered only [39,096, 39,212].
    std::uint32_t session_cell_frontier = 0;
    bool          session_cell_offered  = false;
    std::uint32_t sibling_candidates   = 0;
    std::uint32_t retained_sources     = 0;
    std::uint32_t consumed_sources     = 0;
    // HOW OFTEN THE PLANNER GENERATED A DEMOTE OPTION. The eviction line now says WHY the store refused
    // (`demote_refusal`), and the first burst after that instrument went live showed the store refusing only
    // for `already-on-host` or not at all -- so the remaining question is whether a demote was ever GENERATED
    // and lost on cost. Without this, "no demote was possible" and "no demote was offered" stay
    // indistinguishable, which is the ambiguity that has surrounded this decision all along.
    std::uint32_t demote_options       = 0;

    std::uint64_t initial_predicted_total_ns = 0;
    std::optional<std::uint64_t> first_improvement_ns;
    std::uint32_t incumbent_improvements         = 0;
    std::uint64_t search_work                    = 0;
    std::uint64_t search_granted_ns              = 0;
    std::uint32_t search_renewals                = 0;
    bool search_discovery_used                   = false;
    std::uint64_t search_overshoot_ns            = 0;
    MaterializationSearchPhase search_stop_phase = MaterializationSearchPhase::None;
    bool search_boundary_limited                 = false;

    [[nodiscard]] friend constexpr bool
    operator==(const MaterializationDiagnostics&,
               const MaterializationDiagnostics&) noexcept = default;
};

struct GenerationResult {
    PromptSummary prompt;
    std::vector<TokenId> generated_token_ids;
    std::string content;
    std::string reasoning;
    std::vector<GeneratedToolCall> tool_calls;
    ToolCallParseDiagnostics tool_call_parse;
    std::uint32_t reasoning_tokens = 0;
    FinishReason finish_reason     = FinishReason::None;
    std::optional<std::string> matched_stop_string;
    std::uint32_t reused_prompt_tokens = 0;
    PrefixReusePath prefix_reuse_path  = PrefixReusePath::Root;
    MaterializationDiagnostics materialization;
    GenerationTimings timings;
    GenerationEngineTiming engine_timing;
    SpeculativeStats speculative;
    ThinkingBudgetStats thinking;
};

struct ArenaMemorySummary {
    std::size_t capacity_bytes  = 0;
    std::size_t used_bytes      = 0;
    std::size_t peak_used_bytes = 0;
};

// Logical regions within the one physical workspace allocation. These byte values describe
// layout and live extents and must not be added to workspace.capacity_bytes.
struct VisionWorkspaceMemorySummary {
    std::uint32_t aggregate_prompt_tokens = 0;
    std::uint32_t max_item_tokens         = 0;
    std::size_t general_capacity_bytes    = 0;
    std::size_t encode_peak_bytes         = 0;
    std::size_t handoff_offset_bytes      = 0;
    std::size_t handoff_capacity_bytes    = 0;
    std::size_t handoff_active_bytes      = 0;
    std::size_t handoff_peak_bytes        = 0;
};

struct MemorySummary {
    int device                                = 0;
    std::uint32_t max_context                 = 0;
    KvCapacityMode kv_capacity_mode           = KvCapacityMode::Explicit;
    std::uint32_t kv_capacity                 = 0; // Resolved page-aligned Main KV capacity.
    std::uint32_t kv_capacity_page_groups     = 0;
    std::uint32_t kv_capacity_max_page_groups = 0;
    KvCacheStorage kv_cache                   = KvCacheStorage::BFloat16;
    ArenaMemorySummary weights;
    ArenaMemorySummary sequence;
    ArenaMemorySummary workspace;
    std::optional<VisionWorkspaceMemorySummary> vision_workspace;
    std::size_t minimum_runtime_reservation_bytes = 0;
    std::size_t kv_capacity_increment_bytes       = 0;
    std::size_t runtime_reservation_bytes         = 0;
    std::size_t available_after_weights_bytes     = 0;
    std::size_t available_after_startup_bytes     = 0;
    std::size_t kv_capacity_headroom_bytes        = 0;
    std::size_t planned_slack_bytes               = 0;
    std::size_t workspace_logical_peak_bytes      = 0;
    std::size_t cuda_graph_allowance_bytes        = 0;
    std::size_t kv_payload_bytes                  = 0;
    std::uint32_t host_state_capacity_slots       = 0;
    // THE STATE AXIS'S CEILING AND ITS REFUSALS. `max` is 0 when unbounded. `ceiling_refusals` counts growth
    // refused BY THAT CEILING, apart from refusals by the shared pool -- the two mean opposite things, and
    // folding them together would make the trade-off unreadable. Read `ceiling_refusals` against
    // `host_state_pregrow_refusals` to see whether bounding this axis actually forces state evictions.
    std::uint32_t host_state_max_slots            = 0;
    std::uint64_t host_state_growth_ceiling_refusals = 0;
    std::uint32_t host_state_occupied_slots       = 0;
    std::size_t host_kv_capacity_bytes            = 0;
    // THE SHARED PINNED POOL (2026-09-26). `host_state_capacity_slots` above is now LIVE (it follows the
    // pool), and these say how much the pool holds, how much is unallocated, and whether it has had to grow.
    // `grows == 0` with demotable evictions still happening is the instrument check: it means the growth
    // path never fired, which is indistinguishable from a machine that never needed it.
    std::size_t   host_pinned_capacity_bytes = 0;
    std::size_t   host_pinned_free_bytes     = 0;
    std::uint32_t host_pinned_chunks         = 0;
    std::uint64_t host_pinned_grows          = 0;
    // INCLUDES SPECULATIVE PRE-GROW REFUSALS -- one of the three that still conflate them; see the warning
    // at `host_kv_grow_refusals`. Measured 2026-10-03: this read 1282, exactly the soak's
    // `host_state_pregrow_refusals`, while the SEPARATED `host_pinned_allocation_refusals` stayed 0. Demand
    // policy refusals can be DERIVED: this - `host_state_pregrow_refusals` - `host_kv_pregrow_refusals`.
    std::uint64_t host_pinned_grow_refusals  = 0;
    // THE LARGEST SINGLE FREE RUN, and the only figure that separates "full" from "fragmented":
    // `host_pinned_free_bytes` can be large while nothing large can be placed. The pool has computed this
    // since the elastic-pool change and its own comment says fragmentation "is visible here and nowhere
    // else" -- it simply was never exported, so the question could not be asked of `/stats`.
    std::size_t host_pinned_largest_free_run_bytes = 0;
    // GROWTH HEADROOM ONLY -- NOT capacity plus growth. `max_pinnable()` is `MemAvailable - reserve`
    // (clipped by `max_bytes`/`shmem_cap_bytes` when non-zero), i.e. how much MORE could be pinned, and
    // `host_kv_arena.cpp` spends it exactly that way (`unsatisfied_bytes <= growth_headroom_bytes`). An
    // earlier version of this comment said "pinned capacity plus what growth could add", which would be
    // read as a total; a review caught it.
    // **AND IT IS AS OF THE LAST READING, NOT NOW:** `reading_` is refreshed only at the KV pre-grow or
    // inside `allow()`, so `/stats` can show a figure whose age is unrecorded. Read it as a ceiling the
    // policy will spend, not a live measurement. `PinnedHostPool::growth_headroom_bytes()` reads an INJECTED pure query (0 when
    // none is installed), deliberately separate from the growth policy: the planner calls it inside a
    // bounded search that must have no side effects, and wiring it to `allow()` would re-read /proc/meminfo
    // and move the policy's call counters on every probe. Read 0 as "no query installed OR no headroom" and
    // pair it with `host_pinned_last_veto` to tell those apart -- the same absence-is-not-zero discipline
    // the counters beside it exist for.
    std::size_t host_pinned_growth_headroom_bytes = 0;
    // WHY a growth was refused, and from what reading. `host_pinned_grow_refusals` is a total and the reason
    // had to be DERIVED by reading the source against the running argv (2026-10-01); these make it measured.
    // `growth_policy_refusals` vs `growth_pin_failures` is the distinction that sends a fix to the right
    // place: the policy saying no is a POLICY fact (reserve / ceiling / unreadable meminfo), while
    // `cudaMallocHost` failing is a MACHINE fact. They shared one counter until now.
    // ALSO INCLUDES SPECULATIVE PRE-GROW REFUSALS -- see `host_pinned_grow_refusals` above.
    std::uint64_t host_pinned_growth_policy_refusals = 0;
    std::uint64_t host_pinned_growth_pin_failures    = 0;
    // ALLOCATION failures, split the same way and for the same reason as the growth ones above -- plus the
    // one that actually proves fragmentation.
    // `host_pinned_allocation_fragmented_misses` is THE FRAGMENTATION SIGNATURE: the first placement attempt
    // failed while the pool still held at least that many free BYTES, so the free list and not memory
    // refused it. **The first version of this instrument counted the POST-GROW failure and called that the
    // proof, and a review showed that path is UNREACHABLE** (`grow()` pins a fresh extent of at least the
    // requested size, so the retry always fits): the counter asserting fragmentation was real could never
    // fire, and the real path was counted nowhere.
    std::uint64_t host_pinned_allocation_refusals       = 0;
    std::uint64_t host_pinned_allocation_ram_refusals   = 0;
    // THE PRE-GROW'S OWN REFUSALS, kept out of the two above. A session-start pre-grow pins before any real
    // allocation asked for the room, so counting its refusal as an allocation failure made those counters
    // stop meaning "the pool failed to serve a real demand": measured 2026-10-02, ALL 106 refusals and 132
    // fragmented misses on QA were speculative (`pregrows=0`). Read this beside them, never instead of them.
    std::uint64_t host_pinned_allocation_speculative_refusals = 0;
    std::uint64_t host_pinned_allocation_post_grow_failures = 0;
    std::uint64_t host_pinned_allocation_fragmented_misses  = 0;
    std::size_t   host_pinned_reserve_bytes          = 0;
    std::size_t   host_pinned_last_wanted_bytes      = 0;
    std::size_t   host_pinned_last_veto_mem_available_bytes = 0;
    std::string   host_pinned_last_veto;  // none|nothing_wanted|invalid_reading|reserve|max_bytes|shmem_cap
    // The PLANNING-TIME pre-grow (§3 item 6), separately from the pool's total growth: the pool's
    // `grows` counts host-KV spans and demote-path slots too, so without these three a pre-grow that fired
    // is indistinguishable from one that never did -- and a REFUSAL is invisible unless it is counted,
    // which is how "the budget said no every time" reads exactly like "we never asked".
    std::uint64_t host_state_pregrow_attempts = 0;
    std::uint64_t host_state_pregrows         = 0;
    std::uint64_t host_state_pregrow_refusals = 0;
    // The KV axis's equivalents. `fragmented` is not a failure and NOT a refusal: it counts the pre-grows
    // that FIRED on fragmentation -- the arena held free BYTES but no single step-sized RUN -- and it is
    // incremented inside the `Grew` case, beside `pregrows` (`storage/context.cpp:1789`). An earlier version
    // of this comment said the trigger "declined to grow", which is the EXACT INVERSE and matched the
    // journal label it was read beside (`fragmented_skipped`, since renamed); a live reading of 2 beside
    // `pregrows=2` was taken as two refusals until the increment site was checked. It measures how often
    // growth was triggered by FRAGMENTATION rather than exhaustion -- 1 GiB spans at ~1 s each -- which is
    // the number that says whether the session-start pre-grow is affordable.
    std::uint64_t host_kv_pregrow_attempts    = 0;
    std::uint64_t host_kv_pregrows            = 0;
    std::uint64_t host_kv_pregrow_refusals    = 0;
    std::uint64_t host_kv_pregrow_fragmented  = 0;
    std::uint64_t host_kv_fit_growable         = 0;
    // THE VETO'S DENOMINATOR AND ITS OTHER TWO VERDICTS. `blocked_checks` counts nodes that took the
    // pinned-capacity failure path at all; `pinned`/`growable`/`blocked` are the tri-state's answer. Without
    // the denominator, a `growable` of 0 cannot be told from the veto never running.
    std::uint64_t host_kv_blocked_checks       = 0;
    // HOW MANY BYTES THE BIGGEST VETO WANTED, to go with the count above.
    //
    // IT IS A PROCESS-LIFETIME HIGH-WATER MARK, NOT A PER-REQUEST FIGURE -- an earlier version of this
    // comment said "request-level" and that was wrong: it is only ever raised with `std::max` and is never
    // reset, so a "+0 delta" between two readings means something only because it was still 0.
    //
    // AND A VETOED COMPOSITION DOES NOT RETURN `nullopt` -- that claim was wrong too. `compose_pressure_
    // candidate` returns TRUE with the field set (`pressure.cpp:2381-2412`); it is `seal_materialization`
    // that returns `nullopt`, and the planner DOES read the vetoed value per node
    // (`pressure_planner.cpp:1176`, `:1185`). So a per-candidate figure WAS obtainable at the assessment
    // site, and the per-row field was removed one night after it shipped -- 0 in all 2,339 rows that
    // carried it, which read as "the veto refused nothing" when it meant "this value never reached a row".
    // That removal is defensible for the ROW (the row is built from the original candidate, not the
    // composed copy) but "cannot be per-candidate" overstated it.
    std::uint64_t host_kv_blocked_max_bytes    = 0;
    std::uint64_t host_kv_fit_pinned           = 0;
    std::uint64_t host_kv_fit_blocked          = 0;
    // ***THE HEADROOM-AT-VETO DISCRIMINATOR.*** `blocked_within_headroom` counts `Blocked` verdicts whose
    // shortfall FITTED the headroom that existed AT THAT MOMENT. The rule is `Growable` iff
    // `shortfall <= headroom`, so such a verdict contradicts the rule and cannot be the pool's ceiling --
    // something else refused, and the only such paths are the two DESCRIPTOR guards (the arena's
    // `placement.descriptors_short`, the store's table check), NEITHER OF WHICH IS COUNTED ELSEWHERE.
    // READ IT AGAINST `host_kv_fit_blocked`, its denominator. Zero there = every refusal was the ceiling.
    // Non-zero = the growth path is inert for a reason the plan does not state, and the descriptor TABLES
    // rather than the byte budget are what to fix. `_max_bytes` gives the scale.
    std::uint64_t host_kv_blocked_within_headroom           = 0;
    std::uint64_t host_kv_blocked_within_headroom_max_bytes = 0;
    // ***THE REFUSAL REASON SPLIT, and it is the discriminator the `within_headroom` pair could not be.**
    // `fit_after_suballocation_releases` returns `Blocked` with the SHORTFALL UNSET for both `!valid` and
    // `descriptors_short`, so every such refusal has one shape. These split them by cause, indexed by
    // `HostKVBlockReason`, and THREE of the causes are about the RELEASE LIST THE PLANNER BUILDS -- which
    // is the difference between "grow the descriptor tables" and "the planner is passing bad input", two
    // findings in different layers with no capacity setting between them.
    // DENOMINATOR: `host_kv_blocked_checks` (the veto's own count), because the split is taken in the
    // tri-state call the veto makes, not in the simulation both queries share.
    std::uint64_t host_kv_block_reasons[k_host_kv_block_reason_count] = {};
    // KV's own growth, so a host-KV span added on demand is visible and not only inferable from total
    // capacity. With these three -- grew, could not grow, and the existing `maximal_fallback_selections`
    // (evicted everything) -- the three outcomes a full host tier can produce are DISTINGUISHABLE, which
    // they were not: a failed seal and claim contention used to look identical in the fallback counter.
    std::uint64_t host_kv_grows              = 0;
    // THE GROWTH-SIDE COUNTERS STILL INCLUDE SPECULATIVE PRE-GROW REFUSALS -- this one, the pool's
    // `growth_policy_refusals()` and `HostMemoryBudget::refusals()`. Only the `allocation_*` family was
    // separated (`allocation_speculative_refusals`), because only that family is read as "the pool failed to
    // serve a demand". Measured 2026-10-02: `host_kv_grow_refusals` 106 == `host_kv_pregrow_refusals` 106 ==
    // `host_pinned_grow_refusals` 106 in one soak, and 2195 == 2195 in the next -- the same equality that
    // produced a false "2,195 real refusals" reading on the allocation side. **Do not read these as demand
    // failures**; they are "the tier did not grow", pre-grow included. Threading `speculative` through them
    // is the open item.
    std::uint64_t host_kv_grow_refusals      = 0;
    std::size_t host_kv_occupied_bytes            = 0;
};

// Worker-owned monotonic nanosecond counters. Top-level Host phases are mutually exclusive;
// device_wait_ns is blocked wall time and is intentionally excluded from their sum. Detail values
// are subsets of a top-level phase and must not be added to Host-active time again.
struct RuntimeHostWorkStats {
    std::uint64_t engine_boundary_ns      = 0;
    std::uint64_t program_submit_ns       = 0;
    std::uint64_t program_post_ns         = 0;
    std::uint64_t engine_commit_output_ns = 0;
    std::uint64_t engine_maintenance_ns   = 0;
    std::uint64_t device_wait_ns          = 0;

    std::uint64_t decode_host_ns         = 0;
    std::uint64_t decode_device_wait_ns  = 0;
    std::uint64_t prefill_host_ns        = 0;
    std::uint64_t prefill_device_wait_ns = 0;
    std::uint64_t control_host_ns        = 0;
    std::uint64_t control_device_wait_ns = 0;
    std::uint64_t prefill_units          = 0;
    std::uint64_t control_units          = 0;

    std::uint64_t admission_policy_ns           = 0;
    std::uint64_t context_progress_ns           = 0;
    std::uint64_t stats_publication_ns          = 0;
    std::uint64_t admission_policy_invocations  = 0;
    std::uint64_t context_progress_invocations  = 0;
    std::uint64_t stats_publication_invocations = 0;
};

// Monotonic execution counters, boundary-consistent current gauges, and explicitly named last
// decision observations. Consumers derive interval counters by subtracting two snapshots.
// THE SINGLE SOURCE OF THE `DemoteRefusal` CLAUSE NAMES, in the enum's own declaration order
// (`StateImageStore::DemoteRefusal`, `storage/state_store.h`). It lives HERE rather than beside the enum
// because `/stats` must label a bucket and the serve layer cannot see the state store; the enum's
// `demote_refusal_name` reads THIS table, so the journal's `demote_refusal=` token and the `/stats` key
// cannot disagree about what a clause is called. Hyphens are kept because that token is a documented alert
// string (`demote_refusal=already-on-host`); the `/stats` key substitutes `_` for `-`.
inline constexpr const char* k_demote_refusal_names[] = {
    "none",          "host-tier-disabled", "invalid-handle",   "stale-generation", "not-immutable-checkpoint",
    "not-device-resident", "already-on-host", "pending-replica", "pin-saturated",   "no-host-capacity"};
inline constexpr std::size_t k_demote_refusal_buckets =
    sizeof(k_demote_refusal_names) / sizeof(k_demote_refusal_names[0]);

struct RuntimeStats {
    RuntimeHostWorkStats host_work;
    // Actual prompt tokens evaluated by prefill; reused checkpoint-prefix tokens are excluded.
    std::uint64_t computed_prefill_tokens = 0;
    // Tokens committed by decode rounds; the first token emitted by prefill is excluded.
    std::uint64_t committed_decode_tokens = 0;
    // Decode batch executions and the sum of their batch sizes.
    std::uint64_t decode_rounds             = 0;
    std::uint64_t decode_row_rounds         = 0;
    std::uint32_t running_requests          = 0;
    std::uint32_t prefilling_requests       = 0;
    std::uint32_t decode_ready_requests     = 0;
    std::uint32_t waiting_requests          = 0;
    std::uint32_t materializing_requests    = 0;
    std::uint32_t capture_pending_requests  = 0;
    std::uint32_t terminal_pending_requests = 0;
    std::uint64_t active_captures_completed = 0;
    std::uint64_t active_captures_aborted   = 0;

    std::uint64_t root_selections                    = 0;
    std::uint64_t private_endpoint_selections        = 0;
    std::uint64_t private_turn_closure_selections    = 0;
    std::uint64_t private_response_replay_selections = 0;
    std::uint64_t private_long_anchor_selections     = 0;
    std::uint64_t shared_stable_prefix_selections    = 0;
    std::uint64_t reused_prompt_tokens               = 0;
    std::uint32_t last_selected_frontier_tokens      = 0;

    std::uint64_t state_moves     = 0;
    std::uint64_t state_forks     = 0;
    std::uint64_t state_restores  = 0;
    std::uint64_t state_d2h_count = 0;
    std::uint64_t state_h2d_count = 0;
    std::uint64_t state_d2d_count = 0;
    std::uint64_t state_d2h_bytes = 0;
    std::uint64_t state_h2d_bytes = 0;
    std::uint64_t state_d2d_bytes = 0;
    double state_d2h_seconds      = 0.0;
    double state_h2d_seconds      = 0.0;
    double state_d2d_seconds      = 0.0;

    std::uint64_t main_kv_d2h_pages    = 0;
    std::uint64_t main_kv_h2d_pages    = 0;
    std::uint64_t main_kv_d2d_pages    = 0;
    std::uint64_t main_kv_d2h_bytes    = 0;
    std::uint64_t main_kv_h2d_bytes    = 0;
    std::uint64_t main_kv_d2d_bytes    = 0;
    double main_kv_d2h_seconds         = 0.0;
    double main_kv_h2d_seconds         = 0.0;
    double main_kv_d2d_seconds         = 0.0;
    std::uint64_t backend_kv_d2h_pages = 0;
    std::uint64_t backend_kv_h2d_pages = 0;
    std::uint64_t backend_kv_d2d_pages = 0;
    std::uint64_t backend_kv_d2h_bytes = 0;
    std::uint64_t backend_kv_h2d_bytes = 0;
    std::uint64_t backend_kv_d2d_bytes = 0;
    double backend_kv_d2h_seconds      = 0.0;
    double backend_kv_h2d_seconds      = 0.0;
    double backend_kv_d2d_seconds      = 0.0;

    std::uint64_t pressure_spill_pages                 = 0;
    std::uint64_t partial_tail_cow_pages               = 0;
    std::uint32_t device_state_occupied_slots          = 0;
    // The device state pool's CEILING. `occupied` alone cannot say whether the pool is full, and the
    // absence of this number is what let a saturation check compare against 0 forever -- see
    // `PhysicalUsageSnapshot::device_state_capacity_slots`.
    std::uint32_t device_state_capacity_slots          = 0;
    std::uint32_t host_state_occupied_slots            = 0;
    std::uint32_t device_main_kv_occupied_pages        = 0;
    // The device KV pool's ceiling, in PAGES, so the occupancy above is readable as a fraction.
    std::uint32_t device_main_kv_capacity_pages        = 0;
    std::uint32_t device_backend_kv_capacity_pages     = 0;
    std::uint32_t device_backend_kv_occupied_pages     = 0;
    std::size_t host_kv_occupied_bytes                 = 0;
    // THE PRIVATE-CONTINUATION CATALOG (`--max-private-continuations`, 18 -> 32 on 2026-09-27). This is the
    // occupancy/capacity PAIR, which this pool had NEITHER half of -- host state slots
    // (`host_state_capacity_slots` vs `host_state_occupied_slots`), host KV (`host_kv_capacity_bytes` vs
    // `host_kv_occupied_bytes`) and the pinned pool (`host_pinned_*`) each report both, which is why
    // exhaustion here was invisible. The catalog is NOT the only one missing it: the shared-prefix pool has
    // neither half either, and `device-state-slots` has occupancy without a capacity -- both still unexposed,
    // recorded in `plan.md` §4.
    //
    // Occupancy counts cells that are not `Vacant` -- CATALOGUED, CLAIMED, or `ReservedForActive`, which is
    // an ACTIVE LANE'S OWN future publication cell and not the in-flight capture descriptor (that is a
    // Program address-space slot, `address_capacity = P + S + 1`). Read at STATS PUBLICATION, not on
    // demand: `EngineCore::publish_runtime_stats` fills this snapshot on the engine worker (the same lock
    // planning runs under, `execution_mutex_`) and `/stats` serves the published copy -- so it is as of the
    // last publication (the interval, `--log-stats-interval-ms` = 5 s on prod, but also ~20 event sites), not
    // of the request that reads it.
    //
    // **`occupied == capacity` is NOT evidence that the catalog is binding, and must not be read as one.**
    // Every active lane holds a reserved cell, so occupancy is at least the lane count by construction, and
    // under eviction-to-publish a full catalog is the ordinary steady state -- measured on a run with 99.9%
    // turn-closure reuse. The reading that means something is `pressure_publication_cell_losses`, which is
    // decided per PLANNING RUN against the candidate that lost (the backfill loop can inspect one request
    // several times); occupancy only says whether to look at it.
    std::uint32_t private_catalog_capacity_cells       = 0;
    // WHY PRIVATE CATALOG CELLS WERE EMPTIED, counted by path, because a fork that disappears must be
    // ATTRIBUTED and not inferred. A join proved 231 forks vanished between consecutive requests of the same
    // conversation and ZERO were named by an eviction line -- which left consume-vs-release undecided after two
    // rounds of reasoning. `terminal` is a lane finishing (the ordinary consume and replace), `action` is the
    // pressure path, `cancelled` a cancellation, `cleanup`/`rollback` are failure paths.
    std::uint64_t catalog_cell_clears_terminal         = 0;
    std::uint64_t catalog_cell_clears_action           = 0;
    std::uint64_t catalog_cell_clears_cancelled        = 0;
    std::uint64_t catalog_cell_clears_cleanup          = 0;
    std::uint64_t catalog_cell_clears_rollback         = 0;
    // THE SESSION INDEX'S ERASURES, AND ITS OWN CAPACITY/OCCUPANCY PAIR. `erase_session_if_owner` is what makes
    // a conversation's next turn find no cell and fall back to the shared prefix (23,353 tokens). It had no
    // counter at all, and the cell-clear counters above cannot see it: measured 2026-09-28, 8 session entries
    // erased in one second while those read 0/2/0/0/1 -- different routes clear the session entry and the
    // catalog cell. The reason split is the point: an EVICTION is #6's defect reaching the session index, a
    // CONSUME is the ordinary path taking the endpoint with it. The occupancy pair answers what
    // `session_cell_frontier == 0` cannot: no cell because the index is full, empty, or the entry was erased.
    std::uint64_t session_erasures_eviction            = 0;
    std::uint64_t session_erasures_consume             = 0;
    std::uint32_t session_index_capacity_cells         = 0;
    std::uint32_t session_index_occupied_cells         = 0;
    // Host-resident CHECKPOINTS (a proxy for host state slots) split by whether a session cell or an active
    // lane edge ANCHORS them. NOT a reclaim population: an unanchored entry is still matchable by any future
    // request whose prefix it shares (measured: 76 request-log records reused a private entry that was not
    // their own cell), so reclaiming one costs a rebuild rather than freeing something unused.
    // RENAMED 2026-10-04 to match the code's own term. These were `reachable` / `unanchored`, and the
    // second was MISLEADING IN A WAY THAT COST TIME: it reads as "checkpoints with no ANCHOR", but
    // `long_anchors` are counted INTO the number, so an anchored checkpoint in a holderless slot is
    // included. What is actually counted is whether the SLOT has a holder -- an active edge from a private
    // owner, or an occupied session-index cell -- so the pair is now `held` / `unheld`.
    //
    // Read them as a load-tracking PAIR, never as a leak on their own: `unheld` rises under load and drains
    // when the load stops (measured 23->4 and 26->7 within single processes, 2026-10-04). A rise alone is
    // not evidence of anything; the drain is what says they are a queue rather than an accumulation.
    std::uint32_t host_state_checkpoints_held            = 0;
    std::uint32_t host_state_checkpoints_unheld          = 0;
    std::uint32_t private_catalog_occupied_cells       = 0;
    std::uint64_t pressure_private_owners_degraded     = 0;
    std::uint64_t pressure_private_owners_demoted      = 0;
    // The KV half of a demote-to-host. The counter above fires only on STATE residency, and
    // CheckpointSummary carries no KV-residency field, so a KV-only demotion -- legal under the
    // decided policy ("state/KV pairing is about hits, not eviction") -- was invisible in /stats:
    // a live run showed degraded=2, demoted=0, with 899 pages on host.
    std::uint64_t pressure_private_owners_demoted_kv      = 0;
    std::uint64_t pressure_private_owners_demoted_kv_only = 0;
    std::uint64_t pressure_private_owners_evicted      = 0;
    std::uint64_t pressure_shared_owners_degraded      = 0;
    std::uint64_t pressure_shared_owners_evicted       = 0;
    // A shared slot whose catalogued owner was REPLACED by a new publication (`capture.cpp`, where
    // `replaces_shared` is set and the slot becomes `ReservedReplacement`). It had no counter at all,
    // which meant a run where shared prefixes were replaced was indistinguishable from one where none
    // were -- and the one assertion that would have covered replacement in the suite could not be
    // written for want of it (`pressure_shared_owners_evicted` is the KV-pressure path only, so it
    // reads 0/0 across a replacement).
    std::uint64_t pressure_shared_owners_replaced      = 0;
    // #6: private victims evicted while the host tier had room AT THE DECISION (`demotable`), with its
    // denominator. Capacity only -- it is NOT a statement that the victim could have been demoted
    // (that needs a complete, immutable, settled StateImage), and the print at the eviction site is
    // rate-limited to 8 then every 512th, so these are the only un-muted reading of the item.
    std::uint64_t pressure_private_evictions_demotable = 0;
    std::uint64_t pressure_private_eviction_checks    = 0;
    // #6 (2026-09-27): evictions of a victim that was RESTORABLE **and** whose own host state slots would
    // have fitted the room left. Narrower than `demotable` above, which is a pool-level `<` test and so
    // reports room this victim's own footprint may not fit -- the three post-fix prod evictions were all
    // `demotable=1` at 17/18 slots with host KV 94% full.
    std::uint64_t pressure_evictions_with_victim_room = 0;
    // WHICH AXIS EACH EVICTION RELIEVED, over the whole population rather than the rate-limited print.
    // `device` counts victims that held at least one DEVICE state slot (evicting frees device slots);
    // `host` counts victims that held at least one HOST state slot. A victim can hold both, so the two do
    // NOT sum to the eviction count -- read them beside `pressure_private_eviction_checks`, not against it.
    std::uint64_t pressure_evictions_victim_device_slots = 0;
    std::uint64_t pressure_evictions_victim_host_slots   = 0;
    // THE NEEDED AXIS at each eviction: which axis the PLAN asked for, against `evictions_victim_*` which
    // reports the axis the eviction RELIEVED. An eviction whose relief misses the axis the plan needed is
    // incoherent, and that case is invisible with only one of the two. A plan may need several at once, so
    // these do NOT sum to `private_eviction_checks`, which is their denominator.
    std::uint64_t pressure_evictions_deficit_device_main_kv = 0;
    std::uint64_t pressure_evictions_deficit_device_backend_kv = 0;
    std::uint64_t pressure_evictions_deficit_device_state = 0;
    std::uint64_t pressure_evictions_deficit_host_state = 0;
    std::uint64_t pressure_evictions_deficit_host_kv = 0;
    // THE PER-VICTIM KV HALF: was this victim's OWN device KV demotable to host when it was evicted?
    // `pages` is the denominator (evictions whose victim held device KV pages); `pages_total` is the
    // magnitude destroyed (summed pages). The tri-state splits the denominator by the host KV arena's own
    // answer for exactly those pages: `pinned` = they fit the arena AS IT IS (KV destroyed with host room --
    // the operator's defect), `growable` = they fit only after a growth the planner's pinned-only
    // feasibility site cannot see (`pressure.cpp:2385` -- the plan's own stated failure mode), `blocked` =
    // neither. THE THREE MUST BE READ TOGETHER: `pinned == 0` means "none fitted as-is" only when the other
    // two are non-zero; with all three zero the path was not reached at all, which is what `pages` shows.
    std::uint64_t pressure_evictions_kv_pages       = 0;
    std::uint64_t pressure_evictions_kv_pages_total = 0;
    std::uint64_t pressure_evictions_kv_fit_pinned   = 0;
    std::uint64_t pressure_evictions_kv_fit_growable = 0;
    std::uint64_t pressure_evictions_kv_fit_blocked  = 0;
    // THE MAGNITUDE PER VERDICT. The first real window read `fit_pinned=20` of `pages=45` -- a high defect
    // RATE -- while the victims that fitted were the tiny ones, so the page counts run the other way. Read
    // the two together; a rate quoted alone misstates which direction the defect runs. These sum to
    // `pressure_evictions_kv_pages_total`.
    std::uint64_t pressure_evictions_kv_fit_pinned_pages   = 0;
    std::uint64_t pressure_evictions_kv_fit_growable_pages = 0;
    std::uint64_t pressure_evictions_kv_fit_blocked_pages  = 0;
    // THE PER-VICTIM RECORD. The counters above give a window's TOTALS; two windows of the same suite on the
    // same binary then disagreed by 5.8x on `fit_pinned_pages` (2026-10-04), and a total cannot say WHY --
    // whether one huge victim or many small ones moved it. A per-victim record can: it makes the window a
    // DISTRIBUTION, so the spread is explained rather than merely observed, and a reader can join a victim's
    // size to its verdict instead of inferring size from an aggregate.
    //
    // WHAT IT IS NOT: it is not a replacement for the counters and it is not a window total at a glance. It is
    // a RING of the most recent records, so it never has the "first screenful" bias the rate-limited journal
    // line has; `pressure_eviction_kv_records_total` is how many were ever recorded, so a reader can tell a
    // partial ring from a whole window.
    struct EvictionKVRecord {
        std::uint32_t pages      = 0;  // the victim's RESIDENT owned device KV pages (what a demote would copy)
        std::uint32_t frontier   = 0;  // execution frontier in tokens -- the size proxy a reader joins on
        std::uint8_t  fit        = 0;  // HostKVFit: 0 pinned, 1 growable, 2 blocked (see host_kv_fit_name)
        std::uint8_t  restorable = 0;  // held a complete state image, i.e. a demote would have preserved work
        std::uint8_t  evicted_   = 0;  // reserved; 1 for the shape the rest of the fields describe
    };
    std::array<EvictionKVRecord, k_eviction_kv_record_capacity> pressure_eviction_kv_records{};
    std::uint32_t pressure_eviction_kv_record_count = 0;  // valid entries, oldest first
    std::uint64_t pressure_eviction_kv_records_total = 0;  // ever recorded; > count means the ring wrapped
    // WHICH AXIS REFUSED A PLAN, per axis, out of `physical_peak_fits`'s six-way conjunction. This is the
    // instrument that makes "the host had no room for this victim's share" separable from "the plan was short
    // on a different axis" -- two readings that were IDENTICAL before it and imply opposite fixes. A peak can
    // exceed several capacities at once, so these do NOT sum to the refusal count; the denominator is
    // `pressure_physical_peak_fits_calls`, which is taken before any axis filter.
    std::uint64_t pressure_physical_peak_refusals_by_axis[k_peak_axis_count] = {};
    // METHOD 2's DECISION COUNTERS. `grows` counts seals that pinned the room a `Growable` plan needed;
    // `refusals` counts seals where the pin failed, so the plan fell back to today's behaviour.
    // READ THE PAIR: a `grow` for a plan the search then does not adopt pins memory for nothing, and a
    // `refusal` says admission alone cannot carry the fix. Either one alone is not a verdict.
    std::uint64_t host_kv_seal_grows    = 0;
    std::uint64_t host_kv_seal_refusals = 0;
    // THE HOST-STATE SHORTFALL out of `physical_peak_fits`, which ANDs six axes and so could not say which
    // failed. `fit_checks` is the DENOMINATOR -- calls where the plan asked for host state room at all -- so
    // `shortfall_calls == 0` can be told from "never asked". `shortfall_max_slots` is the worst shortfall IN
    // SLOTS, and it is the number that decides the hypothesis: the only pre-planning growth is ONE slot
    // (`HOST_STATE_PREGROW_SLOTS`), so a maximum above 1 means plans needed more than the pre-grow supplied.
    // Compare it against `memory.host_pinned_growth_headroom_bytes` divided by the slot size to tell "one
    // growth away" from "genuinely capped".
    // EVERY call to the query, before its axis filter. `fit_checks == 0` alone cannot distinguish "host state
    // was never demanded" from "the query never ran"; this separates them, so a quiet reading is attributable.
    std::uint64_t pressure_physical_peak_fits_calls       = 0;
    std::uint64_t pressure_host_state_fit_checks          = 0;
    std::uint64_t pressure_host_state_shortfall_calls     = 0;
    std::uint64_t pressure_host_state_shortfall_max_slots = 0;
    // The device-KV axis's counterparts, same reading rule: denominator, refusals, then the worst shortfall
    // IN PAGES. This axis carries most of the eviction deficit, and its live occupancy reads half empty, so
    // the magnitude is what says whether the peak misses by a little or by a factor.
    std::uint64_t pressure_device_main_kv_fit_checks          = 0;
    std::uint64_t pressure_device_main_kv_shortfall_calls     = 0;
    std::uint64_t pressure_device_main_kv_shortfall_max_pages = 0;
    // THE #6 POPULATION CANDIDATE: `victim_room && demote_possible`. TWO CORRECTIONS FROM THE LOAD'S OWN
    // DATA govern how this reads, both recorded below rather than left in the history that produced them:
    //   * **`already-on-host` is NOT benign.** The rate-limited print shows 7-of-8 of that clause, and an
    //     earlier reading of this file took that to mean 7-of-8 harmless. It does not: state on host +
    //     evicted = the host slots are freed and the copy is DESTROYED, and `restorable=1` means a checkpoint
    //     died. What is moot is the DEMOTE, not the loss.
    //   * **`victim_room` is PER-VICTIM and the plan's need is AGGREGATE.** A burst evicted eight victims at
    //     `host_state_slots` 108/110 to free ~26 slots while each read `victim_room=1` individually. So
    //     `victim_room=1` does NOT mean the eviction was avoidable. This counter is a POPULATION, not the
    //     defect count; the plan-level question it gestures at is UNINSTRUMENTED.
    std::uint64_t pressure_evictions_demote_possible  = 0;
    // THE REFUSAL REASON AT EACH **PRIVATE, MATERIALIZATION-PATH** EVICTION, one bucket per clause of
    // `k_demote_refusal_names` (same order). SCOPE CORRECTED 2026-10-04: this said "every eviction", and it
    // is not -- `capture.cpp` releases private victims through `release_continuation_slot_strict` without
    // touching this counter, and shared evictions are not counted here either. **The sum invariant therefore
    // holds BY CONSTRUCTION against `private_eviction_checks`** (both increment at
    // `materialization.cpp:2334-2335`), which makes it a consistency check on this path and NOT a claim
    // about all evictions.
    // The `demote_refusal=` field was PRINTED and never counted, and that print is rate-limited to the first
    // 8 then every 512th -- so a 59-eviction window yields exactly 8 lines and they are the window's FIRST 8
    // evictions, measured 2026-10-02 as 7-of-8 `already-on-host`. The sample is biased to the start of the
    // window BY CONSTRUCTION; a counter cannot be sampled away.
    // NOTE: the #6 population is NOT attributable here and cannot be -- `demote_possible` IS
    // `demote_refusal == None`, so those evictions are `None` by construction. What this attributes is the
    // OTHER population: the evictions where a demote was NOT available, which is what a fix can be aimed at.
    // DENOMINATOR: `pressure_private_eviction_checks` (every eviction increments exactly one bucket).
    std::uint64_t pressure_demote_refusal_at_eviction[k_demote_refusal_buckets] = {};
    // The OTHER half of #6, and the one that had no signal at all: REQUESTS that lost reuse to the private
    // catalog. A plan takes its publication cell from its own consumed private source, a `Vacant` cell, or a
    // victim it evicts; when every cell is catalogued and no victim is evictable, a candidate that could
    // have been adopted cannot be, and it used to be dropped with no counter, no journal line and no /stats
    // field -- so "the catalog is too small" was indistinguishable from the eleven other reasons the goal
    // builder returns nullopt. `assessed_targets_without_goal` (JSONL, 3e0880bf) does NOT cover it: it is
    // per-request, conflates every reason, and is absent from /stats and from the journal.
    //
    // COUNTED ONCE PER REQUEST, and only when it cost something: the winning plan reused strictly fewer
    // tokens than a candidate that EVERY one of whose goal probes failed on the cell and on nothing else.
    // Both halves of that matter. Counting probe CALLS instead -- the first version of this instrument --
    // reported 108,544 for 24 requests, because the planner probes a cell-less option from five sites on its
    // way to taking an eviction; that number goes nonzero whenever retention fills the catalog, which is the
    // normal state under eviction-to-publish, so it cannot falsify a capacity raise and fires as an alert on
    // healthy traffic. Measured against it: 97 PRINTED LINES -- about 45.6k probes, since the print is
    // rate-limited to 8 then every 512th -- in a run where four requests reused 99.9%.
    // #6's GENERATION half, as a ratio: how often the planner inspected a pressure option, and how often the
    // option it built chose to demote. `demote_refusal` on the eviction line answers "would the store have
    // refused"; these answer "was a demote ever offered". Both are needed: the store refusing and the planner
    // never offering are different defects with different fixes.
    // THE FIVE WAYS AN OPTIONAL CAPTURE IS SKIPPED. Consuming a checkpoint is mandatory (the release happens
    // at activation) and re-creating it is optional, so a conversation's continuation can end up with a deep
    // token ledger and shallow keys -- offering only its old anchors -- and nothing anywhere said why.
    // `not_feasible_no_pressure` is the one that matters most: a private-only capture that did not fit, with NO
    // pressure planning, so nothing was evicted or demoted to make room for it.
    std::uint64_t capture_skips_transaction_or_fork     = 0;
    std::uint64_t capture_skips_cancelled               = 0;
    std::uint64_t capture_skips_nothing_to_publish      = 0;
    std::uint64_t capture_skips_stale_pressure_plan     = 0;
    std::uint64_t capture_skips_not_feasible_no_pressure = 0;
    std::uint64_t pressure_options                     = 0;
    std::uint64_t pressure_demote_options              = 0;
    // WHY `inspect_pressure_option` built no demote option, by clause. Only the third is a policy choice;
    // the total cannot separate them, and a fix aimed at the wrong one would be aimed at nothing.
    std::uint64_t pressure_options_refused_no_kv             = 0;
    // Why the STATE-side demote option died at `pressure.cpp:796`. `no_state_deficit` means the deficit
    // carried no state slots, so the relief wanted is a KV demote -- a different fix from the other six
    // conditions, which say no demote exists and the eviction is correct.
    std::uint64_t pressure_demote_option_refused_no_state_deficit = 0;
    // THE PRESERVATION PROBE. `calls` is the denominator: without it, "no owner lacked an option" and "no
    // owner was examined" read the same.
    //
    // **`pressure_successors_without_option` COUNTS THE EMPTY RESULTS -- the owners that HAD NO non-evicting
    // successor. Its published key was named `successors_with_option` until 2026-10-06, which said the exact
    // OPPOSITE of what it counted, and `plan.md` recorded a wrong reading from it before the inversion was
    // caught.** So the owners that DID have a preserving alternative are
    // `pressure_successor_calls - pressure_successors_without_option`.
    std::uint64_t pressure_successor_calls         = 0;
    std::uint64_t pressure_successors_without_option  = 0;
    // The calls whose successor set was empty BY CONSTRUCTION -- the current decision already evicts
    // (`pressure.cpp:953`). 0 of 1609-1776 on the #6 repro, so it needs no subtraction there; it is carried
    // because on a workload that expands FROM an eviction it would, and the two populations cannot be told
    // apart from `calls` alone.
    std::uint64_t pressure_successor_calls_evicting_current = 0;
    // WHY an empty successor set was empty, one bucket per clause in `k_successor_empty_reason_names` order.
    // **The sum MUST equal `pressure_successors_without_option`; a mismatch means the split is incomplete or a
    // count is misplaced -- which is how the inverted name above was caught** (the buckets summed to 58 while
    // the empty count read 1718, and 58 was the old `with_option`). `no_endpoints` vs `all_refused` are
    // opposite findings about the same population.
    std::uint64_t pressure_successor_empty_by_reason[k_successor_empty_reason_count] = {};
    // #6's LAST QUESTION, QUANTIFIED: pages EXAMINED and rejected as a device-KV relief candidate, by clause
    // and by direction ([0] demote, [1] drop). Not pages relieved -- rejected candidacies, so the number is a
    // function of how much the plan asked for as well as what was available. Read the DEMOTE row against its
    // own total: `active_reference` dominating it is the mechanism `plan.md` names (a demote cannot touch an
    // active victim's KV, so a preserving plan cannot relieve the axis it is short on).
    std::uint64_t pressure_kv_relief_rejected[k_kv_relief_direction_count][k_kv_relief_clause_count] = {};
    // WHAT REACHED THE PEAK. `requested` is the device-KV demand the direction was asked to satisfy; the two
    // directions SHARE ONE BUDGET and run IN ORDER (drop first, then demote on the remainder), so `relieved`
    // against `requested` says whether the demote was ASKED for too little or FOUND too little.
    std::uint64_t pressure_kv_relief_calls[k_kv_relief_direction_count]          = {};
    std::uint64_t pressure_kv_relief_requested[k_kv_relief_direction_count]      = {};
    std::uint64_t pressure_kv_relief_saturated_asks[k_kv_relief_direction_count] = {};
    std::uint64_t pressure_kv_relief_max_ask[k_kv_relief_direction_count]        = {};
    std::uint64_t pressure_kv_relief_relieved[k_kv_relief_direction_count]  = {};
    std::uint64_t pressure_kv_relief_gate_blocks[k_kv_relief_gate_clause_count] = {};
    // THE HOST DEFICIT THAT BLOCKED THE DEMOTE PASS, in bytes. Read beside the veto's `host_kv_fit_*` trio:
    // those read a different quantity, so "the host was never blocked" and "the demote was refused for host
    // bytes" can both be true and are NOT comparable from what is exported.
    // WHICH AXIS KILLED A TARGET, split by whether the target evicts ([0] preserving, [1] evicting). The
    // global `pressure_physical_peak_refusals_by_axis` cannot give this: it sums over every call site, so it
    // cannot say whether the axis that refuses is refusing PRESERVING plans or evicting ones.
    std::uint64_t pressure_peak_refusals_by_preserving[2][k_peak_axis_count] = {};
    // THE PEAK ARITHMETIC, summed over compose steps: `peak = max(0, peak + added - removed)` per axis.
    // `added` is what each step DEMANDS, `removed` what it FREES, `peak_max` the largest resulting peak. The
    // two halves are the model -- an axis that refuses because `added` dwarfs `removed` is a different defect
    // from one where both are large.
    // THE ENTITLEMENT A DEMOTE LEAVES STANDING: the victim's RELEASE figure minus its RESIDENT figure, i.e.
    // the pages an eviction frees that a demote does not. It is the quantity that separates #6's two
    // candidate remedies (the demote's relief vs the reservation itself), so it is reported apart.
    // THE RESERVATION'S COMPOSITION: [term][axis], term 0 = the request's own exclusive entitlement (the
    // BASE), 1 = shared replica additions, 2 = transient source restores; axis 0 = main KV, 1 = backend KV.
    // The peak `physical_peak_fits` judges a plan on IS this reservation, so which TERM dominates decides
    // which remedy is even aimed at the right thing.
    // THE ENTITLEMENT'S OWN SPLIT. `reserved_tokens` IS `prompt_tokens + (output_tokens - 1)`, so these
    // three settle whether the reservation is the prompt, the output budget, or both.
    // THE PLAN-LEVEL RELIEF OF EACH OPTION, same victim, same moment: [0] evicting, [1] preserving. The
    // VICTIM-level figures are equal (gap = 0), so any difference between the plans is HERE.
    // THE PER-INSTANCE EVIDENCE for #6's remaining question, oldest first; `_total` is its denominator and
    // says when the ring wrapped.
    // THE AGGREGATE over ALL assessments, split by class: total, admitted, and refused BY THE HOST-ALLOCATION
    // GATE. The remainder (`total - feasible - host_blocked`) is what the PEAK axes account for; a non-zero
    // remainder that the axis histogram does not cover is an unaccounted refusal.
    // [feasible][preserving]: what AN ADMITTED plan has that a refused one does not -- a bigger relief or a
    // smaller reservation. That is the measurement that separates #6's two remaining candidates.
    std::uint64_t pressure_assessed_agg_count[4]       = {};
    std::uint64_t pressure_assessed_agg_reservation[4] = {};
    std::uint64_t pressure_assessed_agg_removed[4]     = {};
    std::uint64_t pressure_assessed_agg_selected[4]    = {};
    std::uint64_t pressure_assessed_agg_evicting[4]    = {};
    std::uint64_t pressure_assessed_agg_available[4]   = {};
    std::uint64_t pressure_assessed_agg_chosen[4]      = {};
    std::uint64_t pressure_assessed_larger_available[4] = {};
    std::uint64_t pressure_assessed_chose_largest[4]    = {};
    std::uint64_t pressure_assessed_agg_kv_entries[4]   = {};
    // *** THE PER-AXIS SHORTFALL, SUMMED OVER ALL ASSESSMENTS -- NOT THE RING (2026-10-07). ***
    // `pressure_assessed_peak_records` carries the same figure PER ROW, and it is a 64-row WINDOW: arm A35
    // made 5,912 assessments, and the ring read the host axis at 1.51 GiB mid-run and 184 MiB at the end while
    // the population barely changed size. **A magnitude is quoted from HERE**, divided by the matching
    // `pressure_assessed_agg_count[bucket]`; the ring keeps its per-row cross-check against `refusal_mask` and
    // nothing else. `k_peak_axis_names` order -- the axes are PAGES and BYTES, so add nothing across them.
    std::array<std::array<std::uint64_t, k_peak_axis_count>, 4> pressure_assessed_axis_shortfall{};
    // THE VICTIM'S OWN FIGURES AT THE DECISION SITE: the release figure an eviction credits, the resident
    // figure a demote copies, and their difference -- the entitlement a demote does not credit.
    std::uint64_t pressure_victim_ownership_calls        = 0;
    std::uint64_t pressure_victim_ownership_release_sum  = 0;
    std::uint64_t pressure_victim_ownership_resident_sum = 0;
    std::uint64_t pressure_victim_ownership_gap_sum      = 0;
    std::uint64_t pressure_victim_ownership_gap_max      = 0;
    std::uint64_t pressure_assessed_total[2]                = {};
    std::uint64_t pressure_assessed_feasible[2]             = {};
    std::uint64_t pressure_assessed_refused_host_blocked[2] = {};
    std::uint64_t pressure_assessed_peak_records_total = 0;
    std::array<AssessedPeakRecord, k_assessed_peak_record_capacity> pressure_assessed_peak_records{};
    std::uint64_t pressure_successor_relief_preserving_options_seen = 0;
    std::uint64_t pressure_successor_relief_count[2]       = {};
    std::uint64_t pressure_successor_relief_main_kv[2]     = {};
    std::uint64_t pressure_successor_relief_backend_kv[2]  = {};
    std::uint64_t pressure_successor_relief_state_slots[2] = {};
    std::uint64_t pressure_entitlement_split_calls           = 0;
    std::uint64_t pressure_entitlement_split_prompt_tokens   = 0;
    std::uint64_t pressure_entitlement_split_output_tokens   = 0;
    std::uint64_t pressure_entitlement_split_reserved_tokens = 0;
    std::uint64_t pressure_reservation_composition_calls = 0;
    std::uint64_t pressure_reservation_composition[3][2] = {};
    std::uint64_t pressure_victim_entitlement_gap_calls     = 0;
    std::uint64_t pressure_victim_entitlement_gap_pages_sum = 0;
    std::uint64_t pressure_victim_entitlement_gap_pages_max = 0;
    std::uint64_t pressure_peak_model_calls          = 0;
    std::uint64_t pressure_peak_model_added[k_peak_axis_count]    = {};
    std::uint64_t pressure_peak_model_removed[k_peak_axis_count]  = {};
    std::uint64_t pressure_peak_model_peak_max[k_peak_axis_count] = {};
    // THE TEST OF THE READ MECHANISM: is the DEMOTE's relief shorter when the plan had a non-zero host
    // request? PASS 1 (host-duplicate drop) and PASS 2 (device-duplicate drop) require the SAME clause and
    // share one `selected[]`, so PASS 1 should starve PASS 2 -- and the demote's take is the measure.
    std::uint64_t pressure_kv_relief_demote_calls_host_req_zero       = 0;
    std::uint64_t pressure_kv_relief_demote_calls_host_req_nonzero    = 0;
    std::uint64_t pressure_kv_relief_demote_relieved_host_req_zero    = 0;
    std::uint64_t pressure_kv_relief_demote_relieved_host_req_nonzero = 0;
    std::uint64_t pressure_kv_relief_host_blocked_bytes_sum = 0;
    std::uint64_t pressure_kv_relief_host_blocked_bytes_max = 0;
    std::uint64_t pressure_demote_option_refused_precondition     = 0;
    std::uint64_t pressure_options_refused_active_lanes      = 0;
    std::uint64_t pressure_options_refused_evicting_current  = 0;
    std::uint64_t pressure_publication_cell_losses     = 0;

    // SHARED-PREFIX RELEASE REFUSALS, PER CLAUSE. `release_shared_prefix` is `noexcept` and returns early on
    // four grounds, so a refusal kept its occupancy with nobody learning why; the probe that named the reason
    // was behind `NINFER_RELEASE_PROBE`, set in no recorded run. These are the always-on replacement: four
    // counters, read here, with no gate to forget and no rate limit. Zero on each is a RESULT only if the
    // release path ran at all -- these are counters, so unlike a print their zero is a number, not a silence.
    // THE DENOMINATOR. Read it with the four refusals: `attempts == 0` means the release path never ran, so
    // their zeros are UNMEASURED rather than clean -- the same rule as `publication_cell_probes`.
    std::uint64_t shared_prefix_release_attempts                 = 0;
    std::uint64_t shared_prefix_release_refusals_stale_index = 0;
    std::uint64_t shared_prefix_release_refusals_in_flight   = 0;
    std::uint64_t shared_prefix_release_refusals_not_releasable = 0;
    std::uint64_t shared_prefix_release_refusals_check_threw = 0;
    // The DENOMINATOR. Raw goal-probe calls, one or two orders of magnitude larger than any plausible loss
    // count, kept only so that `losses == 0` can be told from a planner that stopped probing. Never read it
    // as the loss: it is search pressure, not an outcome.
    std::uint64_t pressure_publication_cell_probes     = 0;
    // THE DEMAND WINDOW'S SET BITS, BY THE EVIDENCE THAT SET THEM -- see
    // `ResourceManager::tally_demand_evidence`. The portfolio valuation sums a saving per set bit, so if
    // `considered` dominates, `future_loss_ns` is pricing OFFERS as DEMAND: every request that merely had a
    // checkpoint in its candidate set registers demand for it, and the planner then preserves the
    // continuation and refuses reuse to protect demand that may never arrive. Uncapped, because a capped
    // print would hide the very ratio this exists to measure.
    std::uint64_t pressure_demand_bits_selected        = 0;
    std::uint64_t pressure_demand_bits_resident        = 0;
    std::uint64_t pressure_demand_bits_considered      = 0;
    // The at-risk totals, and they exist because the JOURNAL PRINT IS CAPPED -- 8 then every 512th -- so the
    // printed line count is a SAMPLE and never the count. Reading "8 at-risk runs" off eight lines is the
    // documented trap for the eviction print, and it was made here before these fields existed. `at_risk_runs`
    // counts non-winner candidates that hit the cell-only failure; the veto fields say which single condition
    // disqualified each (a goal existed / another failure / would not out-reuse the winner).
    std::uint64_t pressure_publication_cell_at_risk_runs = 0;
    std::uint64_t pressure_publication_cell_veto_goals   = 0;
    std::uint64_t pressure_publication_cell_veto_other   = 0;
    std::uint64_t pressure_publication_cell_veto_reuse   = 0;
    // WHY EVERY GOAL PROBE FAILED, COUNTED UNCONDITIONALLY -- and the gap this closes is why #6 could not be
    // read. The three `veto_*` fields above are only tallied when `at_risk != 0`, i.e. only for a candidate
    // that BOTH failed on the cell and would have out-reused the winner. So a target that failed for any
    // other reason, or that would not have reused more, is counted in `assessed_targets_without_goal` and in
    // NOTHING that says why. Measured 2026-10-01: `assessed_targets_without_goal` was ~4000 of ~4080 targets
    // while every veto field read 0 -- so the record said almost every target had no goal and could not say
    // what took it. That matters for #6 specifically: `feasible_preserving_alternatives` is gated on `goal`,
    // so "was a preserving plan adoptable?" is dominated by WHY goals fail. `cell_only` is the catalog's
    // doing (no publication cell obtainable); `other` is every earlier rejection in `logical_goal_probe`
    // (unknown candidate, source-mode mismatch, unknown owner, slot out of range, entry not Catalogued /
    // wrong id / wrong revision, an active edge, a pinned shared entry).
    std::uint64_t pressure_goal_blocked_cell_only = 0;
    std::uint64_t pressure_goal_blocked_other     = 0;
    std::uint64_t pressure_checkpoints_dropped         = 0;
    std::uint64_t pressure_searches                    = 0;
    std::uint64_t pressure_search_budget_exhaustions   = 0;
    // PLANNING RUNS where the pressure planner hit the TARGET ARENA'S capacity and stopped searching.
    // The sibling above counts "the search spent its own budget", which is the designed stop; this one
    // counts a CAPACITY ceiling. Both are search truncations, and a ceiling reported as a budget stop is
    // a wall that reads as healthy -- which is exactly how the 4096-target arena filled unnoticed and
    // then failed the request (2026-09-28). Zero here means the search never reached the wall; it does
    // NOT mean the wall is far, because the bound is a count, not a duration.
    std::uint64_t pressure_target_arena_truncations    = 0;
    std::uint64_t pressure_maximal_fallback_selections = 0;
    std::uint32_t shared_active_references             = 0;
    std::uint64_t historical_fork_hits                 = 0;
    double actual_context_transfer_seconds             = 0.0;
};

enum class ContextCostPresetSource : std::uint8_t {
    GenericDefault,
    CompiledDefault,
    External,
};

[[nodiscard]] inline constexpr const char*
context_cost_preset_source_name(ContextCostPresetSource source) noexcept {
    switch (source) {
    case ContextCostPresetSource::GenericDefault:
        return "generic-default";
    case ContextCostPresetSource::CompiledDefault:
        return "compiled-default";
    case ContextCostPresetSource::External:
        return "external";
    }
    return "unknown";
}

struct ContextCostSummary {
    ContextCostPresetSource transfer_source = ContextCostPresetSource::GenericDefault;
    ContextCostPresetSource prefill_source  = ContextCostPresetSource::GenericDefault;
    std::string hardware_class;
    std::string prefill_signature;
    std::filesystem::path preset_path;
};

struct LoadSummary {
    std::string architecture;
    std::string model_name;
    std::string cuda_sync_mode;
    std::vector<std::string> weight_formats;
    std::string prefill_signature;
    double load_seconds                = 0.0;
    double upload_seconds              = 0.0;
    std::uint64_t artifact_bytes_read  = 0;
    std::uint64_t host_to_device_bytes = 0;
    std::uint64_t peak_staging_bytes   = 0;
    std::size_t device_object_count    = 0;
    std::size_t host_object_count      = 0;
    ContextCostSummary context_cost;
};

} // namespace ninfer
