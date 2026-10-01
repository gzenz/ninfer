#pragma once

#include "ninfer/types.h"
#include "runtime/contract/execution.h"
#include "runtime/contract/resources.h"
#include "runtime/engine/context_cache/context_cost.h"
#include "runtime/engine/context_cache/materialization_budget.h"
#include "runtime/engine/context_cache/materialization_planner.h"
#include "runtime/engine/context_cache/shared_capture_planner.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ninfer::runtime {

inline constexpr std::uint32_t kInvalidCatalogSlot = std::numeric_limits<std::uint32_t>::max();
// A goal probe that never resolved a candidate (the caller passed an id no candidate carries). Distinct from
// a resolved candidate index of 0, which is why it is a sentinel and not a zero.
inline constexpr std::size_t kNoCandidateIndex = std::numeric_limits<std::size_t>::max();

enum class LogicalLaneState : std::uint8_t {
    Free,
    Materializing,
    Active,
    TerminalPending,
};

struct RetentionObservation {
    RetentionClass retention_class   = RetentionClass::RecentPrivate;
    std::uint64_t selected_hit_count = 0;
    std::uint64_t last_hit_epoch     = 0;
};

struct PolicyObservationKey {
    bool shared            = false;
    std::uint32_t slot     = kInvalidCatalogSlot;
    std::uint64_t owner_id = 0;
    std::uint64_t revision = 0;
    CheckpointRef checkpoint;

    [[nodiscard]] friend constexpr bool operator==(const PolicyObservationKey&,
                                                   const PolicyObservationKey&) noexcept = default;
};

struct CheckpointObservation {
    CheckpointRef checkpoint;
    RetentionObservation observation;
};

// Byte/pages/ns moved by context transfers, split by direction.  The observation loop that
// produces these runs program-level -- `observe_transfers(result)` has no request in scope --
// so this is a PROCESS-monotonic total and is the only form in which the transfers can be
// collected where they happen.  The engine snapshots it when a materialization starts and
// takes the delta at adoption, which is the scope in which ONE request's transfers are
// separable (the engine's context transaction is a single variant, so at most one
// materialization is in flight at a time).
//
// Why it exists: the request log's proc residual is ordered by PATH, not by host phase.  A
// request that resumes a checkpoint pays its HostToDevice restore BEFORE its prefill starts,
// so that time lands in `proc` under no host-phase counter (req#50: 6.07 s proc against a
// 0.59 s reported prefill).  Emitting the restore/demote split beside the phase totals is
// what lets such a record be decomposed instead of guessed at.
struct ContextTransferTotals {
    std::uint64_t host_to_device_ns    = 0;
    std::uint64_t device_to_host_ns    = 0;
    std::uint64_t host_to_device_pages = 0;
    std::uint64_t device_to_host_pages = 0;
    std::uint64_t host_to_device_bytes = 0;
    std::uint64_t device_to_host_bytes = 0;
};

// ResourceManager owns logical policy only.  Every physical feasibility decision and mutation is
// represented by an opaque ModelContract::ResourcePlan sealed against Program::resource_revision().
template <class ModelContract>
class ResourceManager {
public:
    using Program                 = typename ModelContract::Program;
    using PreparedPrompt          = typename ModelContract::PreparedPrompt;
    using RequestBasePlan         = typename ModelContract::RequestBasePlan;
    using AdmissionCandidate      = typename ModelContract::AdmissionCandidate;
    using ResourcePlan            = typename ModelContract::ResourcePlan;
    using PersistentBackfillProof = typename ModelContract::PersistentBackfillProof;
    using SequenceHandle          = typename ModelContract::SequenceHandle;
    using ContinuationHandle      = typename ModelContract::ContinuationHandle;
    using SharedPrefixHandle      = typename ModelContract::SharedPrefixHandle;
    using CaptureOffer            = typename ModelContract::CaptureOffer;
    using ContinuationSummary     = typename ModelContract::ContinuationSummary;
    using SharedPrefixSummary     = typename ModelContract::SharedPrefixSummary;
    using PrefixShortlistKey =
        std::remove_cvref_t<decltype(std::declval<SharedPrefixSummary>().checkpoint.shortlist_key)>;
    using CaptureAssessment                 = typename ModelContract::CaptureAssessment;
    using ProgramActiveCaptureResult        = typename ModelContract::ActiveCaptureResult;
    using CacheSessionKey                   = typename ModelContract::CacheSessionKey;
    using ProgramContextTransactionProgress = typename ModelContract::ContextTransactionProgress;
    using ProgramMaterializationResult      = typename ModelContract::MaterializationResult;
    using StartResult                       = typename ModelContract::StartResult;
    using FinishResult                      = typename ModelContract::FinishResult;
    using AbortResult                       = typename ModelContract::AbortResult;
    using Planner                           = MaterializationPlanner<ModelContract>;
    using CapturePlanner                    = SharedCapturePlanner<ModelContract>;

private:
    // A transaction capability is a point-in-time structural snapshot. An active edge is a
    // durable logical lease on the owner and deliberately does not freeze that snapshot's
    // generation: another reader may change replica residency while the same owner remains live.
    struct ActiveOwnerEdge {
        LogicalOwnerKey owner;
        std::uint32_t slot = kInvalidCatalogSlot;

        [[nodiscard]] friend constexpr bool operator==(ActiveOwnerEdge,
                                                       ActiveOwnerEdge) noexcept = default;
    };

    struct OwnerClaim {
        PlanningOwnerId planning_id;
        CatalogCapability capability;
        VictimDisposition disposition = VictimDisposition::Retained;
        std::vector<CheckpointRef> dropped_checkpoints;
    };

    struct PlanningOwnerRecord {
        PlanningOwnerId id;
        CatalogCapability capability;
    };

public:
    struct ReuseDomainId {
        std::uint64_t low  = 0;
        std::uint64_t high = 0;

        [[nodiscard]] friend constexpr bool operator==(ReuseDomainId,
                                                       ReuseDomainId) noexcept = default;
    };

    struct PrefixDemandRecord {
        ReuseDomainId domain;
        std::vector<PrefixShortlistKey> candidate_keys;
        std::vector<PrefixShortlistKey> exact_resident_keys;
        std::optional<PrefixShortlistKey> selected_source_key;
    };

    enum class CatalogState : std::uint8_t {
        Vacant,
        Catalogued,
        Claimed,
        ReservedForActive,
    };

    enum class SharedCatalogState : std::uint8_t {
        Vacant,
        Catalogued,
        Claimed,
        ReservedCapture,
    };

    class Choice {
    public:
        Choice(Choice&&) noexcept        = default;
        Choice& operator=(Choice&&)      = delete;
        Choice(const Choice&)            = delete;
        Choice& operator=(const Choice&) = delete;

        [[nodiscard]] const RequestPlanSummary& summary() const noexcept {
            return plan_->summary();
        }

        [[nodiscard]] LaneId destination() const noexcept { return destination_; }

        [[nodiscard]] bool needs_transfer() const noexcept { return plan_->needs_transfer(); }

        [[nodiscard]] ProgramResourceRevision resource_revision() const noexcept {
            return plan_->resource_revision();
        }

    private:
        Choice(LaneId destination, ResourcePlan&& plan, std::uint32_t catalog_capacity,
               std::optional<CacheSessionKey> session, RetentionClass retention,
               bool update_session_index, std::uint64_t publication_order)
            : destination_(destination), plan_(std::move(plan)), session_(std::move(session)),
              retention_(retention), update_session_index_(update_session_index),
              publication_order_(publication_order) {
            private_claims_.reserve(catalog_capacity);
            shared_claims_.reserve(catalog_capacity);
        }

        LaneId destination_{};
        std::optional<ResourcePlan> plan_;
        PrivateSourceMode source_mode_ = PrivateSourceMode::ConsumeToActive;
        std::optional<CatalogCapability> private_source_;
        std::optional<CatalogCapability> shared_source_;
        std::uint32_t publication_slot_ = kInvalidCatalogSlot;
        std::vector<OwnerClaim> private_claims_;
        std::vector<OwnerClaim> shared_claims_;
        std::optional<PolicyObservationKey> selected_observation_;
        std::optional<CacheSessionKey> session_;
        RetentionClass retention_        = RetentionClass::RecentPrivate;
        bool update_session_index_       = true;
        std::uint64_t publication_order_ = 0;
        MaterializationDiagnostics diagnostics_;
        PrefixDemandRecord demand_;

        friend class ResourceManager;
    };

    class PublishedActivation {
    public:
        PublishedActivation(PublishedActivation&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), result_(std::move(other.result_)),
              destination_(other.destination_) {}

        PublishedActivation& operator=(PublishedActivation&&)      = delete;
        PublishedActivation(const PublishedActivation&)            = delete;
        PublishedActivation& operator=(const PublishedActivation&) = delete;

        [[nodiscard]] const SequenceHandle& sequence() const {
            if (!result_) { throw std::logic_error("published activation is empty"); }
            return result_->sequence;
        }

    private:
        PublishedActivation(ResourceManager& owner, StartResult&& result, LaneId destination)
            : owner_(&owner), result_(std::move(result)), destination_(destination) {}

        ResourceManager* owner_ = nullptr;
        std::optional<StartResult> result_;
        LaneId destination_{};

        friend class ResourceManager;
    };

    struct MaterializationOutcome {
        ContextTransactionStatus status = ContextTransactionStatus::Aborted;
        std::optional<PublishedActivation> activation;
        MaterializationDiagnostics diagnostics;
    };

    enum class MaterializationReserveResult : std::uint8_t {
        Reserved,
        Stale,
        Aborted,
    };

    enum class ActiveCaptureReserveResult : std::uint8_t {
        Reserved,
        Skipped,
    };

    struct ActiveCaptureOutcome {
        ContextTransactionStatus status = ContextTransactionStatus::Aborted;
    };

    using ContextTransactionOutcome =
        std::variant<ContextTransactionInProgress, MaterializationOutcome, ActiveCaptureOutcome>;

    struct Inspection {
        Readiness readiness = Readiness::TemporarilyBlocked;
        std::optional<Choice> choice;
    };

    ResourceManager(std::uint32_t lane_count, std::uint32_t private_catalog_capacity,
                    std::uint32_t shared_catalog_capacity, bool cache_enabled,
                    std::uint32_t max_long_anchors, ContextMachineCostModel cost_model)
        : lane_count_(lane_count), catalog_count_(private_catalog_capacity),
          shared_catalog_count_(shared_catalog_capacity), cache_enabled_(cache_enabled),
          catalog_(private_catalog_capacity), shared_catalog_(shared_catalog_capacity),
          session_index_(private_catalog_capacity),
          prefix_index_(checked_prefix_index_capacity(private_catalog_capacity,
                                                      shared_catalog_capacity, max_long_anchors)),
          max_long_anchors_(max_long_anchors), cost_model_(std::move(cost_model)) {
        if (lane_count == 0 || lane_count > kMaximumConcurrency ||
            private_catalog_capacity < lane_count) {
            throw std::invalid_argument("logical resource-manager bounds are invalid");
        }
        const std::size_t observation_capacity = 3U + max_long_anchors_;
        observation_scratch_.reserve(observation_capacity);
        demand_window_.reserve(kDemandWindowCapacity);
        for (CatalogEntry& entry : catalog_) {
            entry.summary.long_anchors.reserve(max_long_anchors_);
            entry.observations.reserve(observation_capacity);
        }
        for (std::uint32_t lane = 0; lane < lane_count_; ++lane) {
            active_[lane].shared_sources.reserve(shared_catalog_capacity);
        }
    }

    // THE DEPTH AT WHICH TO CAPTURE A CHECKPOINT, computed BEFORE the base plan is built -- which is the only
    // point from which it can be made correct. Two earlier attempts failed for two different reasons, and the
    // second is instructive: injecting the group into the CANDIDATE copy produced a group with NO identity and
    // no pricing, and every capture group must carry a `PreparedCaptureIdentity` (`backing`, work at its
    // frontier, per-frontier digests) or the engine throws `planned capture identity is invalid` and recovers
    // (observed on prod 2026-09-27, followed by a cascade of `prepared prompt is empty`). Injecting here, via
    // the base build's own `add_capture`, means the group passes through the identity AND pricing passes like
    // every client marker.
    //
    // The condition is the measured one: this prompt matches stored content DEEPER than any checkpoint below it
    // can resume from, so the matched tail is re-prefilled (28,564 against 23,353 on the shared class; 45,717
    // against 31,229 on the private one, both with the identity chain agreeing).
    [[nodiscard]] std::optional<std::uint32_t> branch_anchor_frontier(const Program& program,
                                                                      const PreparedPrompt& prompt) const {
        std::uint32_t best = 0;
        const auto consider = [&](const Program::PrefixSplit& split) {
            if (split.tokens > split.restorable && split.tokens > best) { best = split.tokens; }
        };
        for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
            const CatalogEntry& entry = catalog_[slot];
            if (entry.state != CatalogState::Catalogued || !entry.handle) { continue; }
            consider(program.prefix_split(*entry.handle, prompt));
        }
        for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
            const SharedCatalogEntry& entry = shared_catalog_[slot];
            if (entry.state != SharedCatalogState::Catalogued || !entry.handle) { continue; }
            consider(program.prefix_split(*entry.handle, prompt));
        }
        if (best == 0) { return std::nullopt; }
        return best;
    }

    [[nodiscard]] Inspection inspect(Program& program, const PreparedPrompt& prompt,
                                     const RequestBasePlan& base, std::uint64_t publication_order,
                                     PlanningAllowance allowance = {}) {
        if (!std::holds_alternative<std::monostate>(transaction_) ||
            program.has_context_transaction()) {
            return {.readiness = Readiness::TemporarilyBlocked};
        }
        if (publication_order == 0) {
            throw std::invalid_argument("request publication order is zero");
        }
        if (!program.isolated_request_feasible(base)) {
            return {.readiness = Readiness::PermanentlyInfeasible};
        }
        std::optional<LaneId> destination;
        for (std::uint32_t lane = 0; lane < lane_count_; ++lane) {
            if (lanes_[lane] == LogicalLaneState::Free) {
                destination = LaneId{lane};
                break;
            }
        }
        if (!destination) { return {.readiness = Readiness::TemporarilyBlocked}; }

        const typename Planner::Clock::time_point planning_started = Planner::Clock::now();
        // THE CANDGEN DIAGNOSTIC GETS ITS OWN GATE. Every `cdbg_log` site in this file (10 of them) is a
        // `[candgen]` line, and it was reachable only through `NINFER_MAT_DEBUG` -- which also switches on the
        // planner's `[mat-debug]` probes: 1.7 MB of output for one prod4 run, 678 MB of serve log, and all of
        // it on the paths the suite TIMES. So the one diagnostic that answers "was the session's own cell even
        // found" could not be enabled on a serving instance, which is exactly where the question was asked
        // (2026-09-27: a third of requests after a restart reused a shared prefix for 23,353 tokens with NO
        // private candidate, and three same-shaped requests took a root instead -- 0% -- and the difference is
        // visible in these lines). `getenv` is non-NULL for an EMPTY string, so an exported empty name turns
        // it ON; that is deliberate here and is why the test harness unsets rather than blanks MAT_*.
        const bool cdbg = std::getenv("NINFER_MAT_DEBUG") != nullptr ||
                          std::getenv("NINFER_CANDGEN_DEBUG") != nullptr;
        // RATE-LIMITED, and that is the whole point of this change (2026-09-27). Turning the gate on used to
        // emit EVERY line: one nine-minute prod window produced 468,786 `[candgen]` lines out of 470,661
        // journal lines, and journald responded by suppressing 28,058 messages -- which can drop `WORKER OOM`
        // and the eviction line, with the watcher blind to the loss (`grep -i suppress tools/ops/ninfer-watch.*`
        // finds nothing). A diagnostic that hides alerts is worse than no diagnostic, so the first 8 and then
        // every 512th is the policy here too -- the same split the eviction print uses. The un-muted readings
        // live in `/stats` and in the request record, which is where a rate limit cannot reach.
        static std::uint64_t cdbg_seen = 0;
        const auto cdbg_log = [cdbg](const char* fmt, ...) {
            if (!cdbg) { return; }
            ++cdbg_seen;
            if (cdbg_seen > 8U && cdbg_seen % 512U != 0U) { return; }
            std::va_list ap;
            va_start(ap, fmt);
            std::vfprintf(stderr, fmt, ap);
            va_end(ap);
            std::fflush(stderr);
        };
        rebuild_prefix_index();
        PrefixDemandRecord provisional_demand;
        provisional_demand.domain =
            reuse_domain(base.context_cache().session_key, publication_order);
        provisional_demand.candidate_keys.reserve(base.context_cache().opportunities.size());
        provisional_demand.exact_resident_keys.reserve(prefix_index_.size());
        if (cache_enabled_) {
            for (const auto& opportunity : base.context_cache().opportunities) {
                if (opportunity.kind != PromptCacheMarkerKind::SharedStablePrefix) { continue; }
                const std::optional<PrefixShortlistKey> key =
                    base.prefix_shortlist_key(opportunity.frontier);
                if (key) { append_unique(provisional_demand.candidate_keys, *key); }
            }
        }
        std::optional<std::size_t> current_session_cell;
        if (cache_enabled_ && base.context_cache().session_key &&
            base.context_cache().update_session_index) {
            current_session_cell = find_session_cell(*base.context_cache().session_key);
        }
        CandidateCounters candidate_counters;
        std::vector<Candidate> candidates;
        candidates.reserve(1U + prefix_index_.size());
        std::optional<AdmissionCandidate> root = program.inspect_admission(
            prompt, base, *destination, nullptr, nullptr, std::nullopt, false);
        if (!root) { throw std::logic_error("Program rejected isolated root planning"); }
        candidates.push_back(Candidate{.plan = std::move(*root)});

        if (cache_enabled_) {
            const auto seshash = [](const std::string_view sv) noexcept {
                std::uint64_t h = 0;
                for (const char c : sv) { h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL; }
                return h;
            };
            {
                const auto probe1 = base.prefix_shortlist_key(1);
                std::uint64_t sfp = 0;
                if (base.context_cache().session_key) {
                    sfp = seshash(base.context_cache().session_key->view());
                }
                cdbg_log("[candgen] === episode prefix_index=%zu session=%016lx digests=%d base_tag=%u sl_size=%zu ===\n",
                         prefix_index_.size(), sfp, probe1 ? 1 : 0, probe1 ? probe1->identity_tag : 0U,
                         base.prefix_shortlist_size());
                if (current_session_cell) {
                    const auto& scell = session_index_[*current_session_cell];
                    if (scell.slot < catalog_count_) {
                        const CatalogEntry& oe = catalog_[scell.slot];
                        cdbg_log("[candgen] OWN slot=%u state=%d handle=%d id=%lu rev=%lu sl_size=%zu\n",
                                 scell.slot, static_cast<int>(oe.state), oe.handle ? 1 : 0,
                                 oe.id, oe.revision, base.prefix_shortlist_size());
                    } else {
                        cdbg_log("[candgen] OWN slot=ABSENT sl_size=%zu\n",
                                 base.prefix_shortlist_size());
                    }
                } else {
                    cdbg_log("[candgen] OWN cell=NONE update_index=%d session=%d sl_size=%zu\n",
                             base.context_cache().update_session_index ? 1 : 0,
                             base.context_cache().session_key ? 1 : 0,
                             base.prefix_shortlist_size());
                }
            }
            // The session cell's slot, resolved BEFORE the loop so each skip site below can say whether the entry
            // it just dropped was the conversation's OWN. Resolving it after the loop would report the outcome
            // without the cause, which is the shape that has cost this work several cycles already.
            static constexpr std::uint32_t kNoSessionSlot = 0xFFFFFFFFu;
            const std::uint32_t session_slot =
                (current_session_cell && session_index_[*current_session_cell].slot < catalog_count_)
                    ? session_index_[*current_session_cell].slot
                    : kNoSessionSlot;
            if (session_slot != kNoSessionSlot) {
                // Provisional: "the slot never appeared in the index". Every site below overwrites it, so a
                // final reason of 2 means the loop genuinely never met this slot.
                candidate_counters.session_cell_skip = 2;
            }
            // A PRIVATE SLOT NUMBER IS NOT A SHARED SLOT NUMBER, and the first version of this treated them as
            // one. `rebuild_prefix_index` appends every shared entry AFTER every private one, so shared entries
            // are visited last; both namespaces start at 0; and the three key sites below run before the
            // `if (!index.shared)` branch. A shared entry whose key missed this prompt could therefore write
            // its reason into the private session cell's field -- producing a plausible-looking 3/4/5 for a
            // cell that was in fact offered. Every reason below is therefore taken ONLY from a private entry.
            //
            // TWO FIELDS, because one slot can hold several index entries (endpoint, rewrite, each long
            // anchor) and they compete: an endpoint skipped for a reason and a shallower anchor offered
            // afterwards would otherwise report a bare `1`, hiding exactly the question this instrument exists
            // to answer. `session_cell_skip` answers "did the cell produce a candidate"; `session_endpoint_skip`
            // answers what happened to the conversation's OWN endpoint.
            //
            // UNVERIFIED BY TEST, and that is recorded rather than implied: a red control for the shared-slot
            // collision was attempted and WITHDRAWN, because it passed with the guard removed -- the fixture
            // never produced the slot-number equality the bug needs, so it proved nothing. The fix rests on the
            // code order above (shared appended after private, both numbering from 0, key sites before the
            // `if (!index.shared)` branch) and on no run. A control that cannot fail is worse than none: it
            // reads as verification.
            // THE SENTINEL COLLISION, and it is the same value on both sides: `kNoSessionSlot` is 0xFFFFFFFF
            // and `PrefixIndexEntry::slot` defaults to `kInvalidCatalogSlot`, which is
            // `numeric_limits<uint32_t>::max()` -- the same number. `prefix_index_` is a FIXED array filled
            // only at its head, so a request with no session cell used to match every UNOCCUPIED tail entry
            // (`index.slot != session_slot` was 0xFFFFFFFF != 0xFFFFFFFF, i.e. false) and write reason 9 --
            // making "failed validation" the reading on every request without a session key, the Bash
            // classifier's traffic included, while the documented 0 was unreachable. The first version also
            // indexed `catalog_[0xFFFFFFFF]` on that path and segfaulted the whole suite; the guard added for
            // that stopped the crash and left the misreport, which is the wrong repair -- the sentinel is what
            // made it misreport, not the slot's trustworthiness. NOTE the two guards below are REDUNDANT:
            // `!index.occupied` alone closes the collision, because an unoccupied entry is exactly what carries
            // the colliding slot default. Both are kept so the intent reads directly, but a control that removes
            // only one of them will not fail, and the one in the suite pins only their union.
            const auto note_session_slot = [&](const PrefixIndexEntry& index, std::uint8_t reason) {
                if (session_slot == kNoSessionSlot || !index.occupied || index.shared ||
                    index.slot != session_slot) {
                    return;
                }
                const CatalogEntry& own = catalog_[index.slot];
                if (own.summary.endpoint && index.checkpoint == own.summary.endpoint->ref) {
                    candidate_counters.session_endpoint_skip = reason;
                }
                if (reason == 1) {
                    candidate_counters.session_cell_skip = 1;  // offered is sticky: later entries cannot undo it
                } else if (candidate_counters.session_cell_skip != 1) {
                    candidate_counters.session_cell_skip = reason;
                }
            };
            for (const PrefixIndexEntry& index : prefix_index_) {
                if (!valid_prefix_index_entry(index)) {
                    // 9, not the provisional 2: the slot IS in the index but failed validation here, and a
                    // reader must be able to tell that from "no index entry at all".
                    note_session_slot(index, 9);
                    continue;
                }
                const std::optional<PrefixShortlistKey> incoming =
                    base.prefix_shortlist_key(index.key.frontier);
                if (!incoming) {
                    const CatalogEntry& nslot = catalog_[index.slot];
                    const std::uint64_t nh =
                        nslot.session ? seshash(nslot.session->view()) : 0ULL;
                    const bool nown = nslot.session && base.context_cache().session_key &&
                                      *nslot.session == *base.context_cache().session_key;
                    cdbg_log("[candgen] priv SKIP slot=%u NULLOPT idx_frontier=%u shared=%d "
                             "sl_size=%zu sess=%016lx %s\n",
                             index.slot, index.key.frontier, index.shared ? 1 : 0,
                             base.prefix_shortlist_size(), nh, nown ? "OWN" : "XSESSION");
                    note_session_slot(index, 3);
                    continue;
                }
                if (incoming->identity_tag != index.key.identity_tag) {
                    cdbg_log("[candgen] priv SKIP slot=%u TAG-MISMATCH base=%u idx=%u f=%u\n",
                             index.slot, incoming->identity_tag, index.key.identity_tag,
                             index.key.frontier);
                    note_session_slot(index, 4);
                    continue;
                }
                if (*incoming != index.key) {
                    // Own-session guard: a re-touch probes every stored slot, so a
                    // *different* session's response naturally mismatches a slot it does
                    // not own. Label those XSESSION-MISMATCH so they are not mistaken for a
                    // real key-match failure of the re-touch's own checkpoint.
                    const CatalogEntry& probe_entry = catalog_[index.slot];
                    const bool own_session = probe_entry.session &&
                                             base.context_cache().session_key &&
                                             *probe_entry.session ==
                                                 *base.context_cache().session_key;
                    cdbg_log("[candgen] priv SKIP slot=%u %s frontier=%u "
                             "in=(%lx,%lx) st=(%lx,%lx)\n",
                             index.slot,
                             own_session ? "DIGEST-MISMATCH" : "XSESSION-MISMATCH",
                             index.key.frontier,
                             incoming->digests[0], incoming->digests[1],
                             index.key.digests[0], index.key.digests[1]);
                    note_session_slot(index, 5);
                    continue;
                }

                if (!index.shared) {
                    const CatalogEntry& entry = catalog_[index.slot];
                    if (entry.state != CatalogState::Catalogued || !entry.handle ||
                        private_has_active_edge(index.slot)) {
                        const bool held_by_active_edge = private_has_active_edge(index.slot);
                        cdbg_log("[candgen] priv SKIP slot=%u state=%d handle=%d active_edge=%d "
                                 "(key matched)\n",
                                 index.slot, static_cast<int>(entry.state),
                                 entry.handle ? 1 : 0,
                                 held_by_active_edge ? 1 : 0);
                        // 7 only. The validator above (`valid_prefix_index_entry`) already requires
                        // `state == Catalogued && handle`, so the state/handle arm of this ternary was
                        // unreachable and code 6 could never be emitted -- a reason in the table that no
                        // input could produce.
                        note_session_slot(index, 7);
                        continue;
                    }
                    // THE SIBLING CASE (2026-09-27). `retain` above is false exactly when the entry IS this
                    // request's own session's -- so the request CONSUMES it, and the plan is Replace, which
                    // destroys the entry's endpoint (`request_plan.cpp` Replace; the consume at
                    // `storage/context.cpp`). That is right for the conversation's next turn and WRONG for a
                    // SIBLING: a concurrent request built from the same prompt plus a small delta resumes the
                    // same checkpoint, leaves the endpoint ledger before the endpoint, and takes the endpoint
                    // with it. The real next turn then finds nothing at its own depth and falls back to the
                    // shared marker's 23,353.
                    //
                    // Demonstrated live from traffic, per key (2026-09-27, `request.session_key`):
                    //   prompt 36225 -> private_endpoint reuse 31160
                    //   prompt 36363 -> private_endpoint reuse 31160   (+138: the sibling, same checkpoint)
                    //   prompt 38472 -> shared_stable_prefix 23355     (the next turn, endpoint gone)
                    // and 15 of the 55 ceiling requests have `split_best_tokens - 1` equal to an earlier
                    // request's reuse point, in 15/15 cases a `private_response_replay`; 196 of 218 such
                    // requests arrived BEFORE the request they extend finished, so they cannot contain its
                    // response -- they are siblings, not next turns.
                    //
                    // The condition is the one that was measured: the request's whole prompt is SHORTER than
                    // the entry's endpoint frontier, so consuming the entry would discard an endpoint this
                    // request can never reach. Retain instead, and the endpoint survives for the turn that
                    // needs it. GATED, because it is unproven and its cost is real: Retain needs a publication
                    // cell where Replace took the source's own, and cells are what prod is short of.
                    const bool endpoint_beyond_prompt =
                        entry.summary.endpoint &&
                        base.summary().prompt_tokens < entry.summary.endpoint->ref.frontier;
                    // COUNTED EVEN WHILE THE BEHAVIOUR IS OFF: this sizes the population the sibling fix would
                    // touch, from traffic, before anything changes. The audit's chain says such a request takes
                    // `Replace` and destroys the conversation's endpoint; `retained_sources`/`consumed_sources`
                    // verify that from the decision rather than from the code.
                    if (endpoint_beyond_prompt) { ++candidate_counters.sibling_candidates; }
                    static const bool sibling_retain = std::getenv("NINFER_SIBLING_RETAIN") != nullptr;
                    const bool retain =
                        (entry.session && (!base.context_cache().session_key ||
                                           *entry.session != *base.context_cache().session_key ||
                                           !base.context_cache().update_session_index)) ||
                        (sibling_retain && endpoint_beyond_prompt);
                    std::optional<AdmissionCandidate> plan =
                        program.inspect_admission(prompt, base, *destination, &*entry.handle,
                                                  nullptr, index.checkpoint, retain);
                    if (!plan) {
                        cdbg_log("[candgen] priv SKIP slot=%u inspect_admission=nullopt\n",
                                 index.slot);
                        note_session_slot(index, 8);
                        continue;
                    }
                    cdbg_log("[candgen] priv BUILD slot=%u reuse_tok=%u retain=%d\n", index.slot,
                             static_cast<unsigned>(plan->summary().reusable_prompt_tokens),
                             retain ? 1 : 0);
                    if (retain) { ++candidate_counters.retained_sources; } else { ++candidate_counters.consumed_sources; }
                    if (plan->summary().reusable_prompt_tokens == 0 ||
                        (retain &&
                         plan->identity_assessment().source_mode != PrivateSourceMode::Retain)) {
                        throw std::logic_error("Program returned an invalid private candidate");
                    }
                    const bool current_session_binding =
                        current_session_cell &&
                        session_index_[*current_session_cell].slot == index.slot &&
                        session_index_[*current_session_cell].owner_id == entry.id &&
                        session_index_[*current_session_cell].revision == entry.revision;
                    append_unique(provisional_demand.exact_resident_keys, index.key);
                    note_session_slot(index, 1);
                    candidates.push_back(Candidate{
                        .plan                    = std::move(*plan),
                        .current_session_binding = current_session_binding,
                        .private_source          = private_capability(index.slot),
                        .selected_observation =
                            PolicyObservationKey{
                                .shared     = false,
                                .slot       = index.slot,
                                .owner_id   = entry.id,
                                .revision   = entry.revision,
                                .checkpoint = index.checkpoint,
                            },
                        .source_key = index.key,
                    });
                    continue;
                }

                const SharedCatalogEntry& entry = shared_catalog_[index.slot];
                if (entry.state != SharedCatalogState::Catalogued || !entry.handle) { continue; }
                std::optional<AdmissionCandidate> plan = program.inspect_admission(
                    prompt, base, *destination, nullptr, &*entry.handle, index.checkpoint, false);
                if (!plan) { continue; }
                if (plan->summary().reusable_prompt_tokens == 0 ||
                    plan->identity_assessment().source_mode != PrivateSourceMode::Retain) {
                    throw std::logic_error("Program returned an invalid shared candidate");
                }
                append_unique(provisional_demand.exact_resident_keys, index.key);
                candidates.push_back(Candidate{
                    .plan          = std::move(*plan),
                    .shared_source = shared_capability(index.slot),
                    .selected_observation =
                        PolicyObservationKey{
                            .shared     = true,
                            .slot       = index.slot,
                            .owner_id   = entry.id,
                            .revision   = entry.revision,
                            .checkpoint = index.checkpoint,
                        },
                    .source_key = index.key,
                });
            }
        }

        std::optional<Choice> selected =
            plan_materialization(program, prompt, base, *destination, candidates, candidate_counters,
                                 publication_order,
                                 planning_started, provisional_demand, allowance);
        if (!selected) {
            // "No plan" is only *temporary* when something can change the answer: an occupied lane
            // finishing, or a reclaimable owner. With every lane Free and no context transaction in
            // flight (checked on entry), nothing can, so the verdict is final.
            //
            // This is the wedge's stage two (#14/#9, 2026-09-25): a recovery left ~958 device pages and
            // one host state slot owned by nothing, so a request above the remaining capacity planned to
            // nothing -- and because feasibility is asked against full capacity, it was never called
            // infeasible either. The head then waited out its 900 s deadline, blocking every request
            // queued behind it, and the only resolver was a restart. Reporting the unsatisfiable block
            // as PermanentlyInfeasible turns it into the capacity error the engine already raises for
            // that enum, at the moment it is decided rather than after a grace period.
            //
            // Safe in the other direction: `Materializing` and `TerminalPending` are not Free, so any
            // lane that could still free or take capacity keeps the request temporarily blocked.
            const bool any_lane_occupied =
                std::any_of(lanes_.begin(), lanes_.end(), [](LogicalLaneState state) {
                    return state != LogicalLaneState::Free;
                });
            if (!any_lane_occupied) { return {.readiness = Readiness::PermanentlyInfeasible}; }
            return {.readiness = Readiness::TemporarilyBlocked};
        }
        return {
            .readiness = selected->needs_transfer() ? Readiness::NeedsTransfer : Readiness::Ready,
            .choice    = std::move(selected),
        };
    }

    [[nodiscard]] std::optional<PersistentBackfillProof>
    prove_persistent_backfill(Program& program, const RequestBasePlan& blocked_head,
                              const Choice& candidate,
                              std::span<const SequenceHandle> persistent_borrowers) const {
        if (!candidate.plan_ || candidate.resource_revision() != program.resource_revision()) {
            return std::nullopt;
        }
        return program.prove_persistent_backfill(blocked_head, *candidate.plan_,
                                                 persistent_borrowers);
    }

    [[nodiscard]] MaterializationReserveResult
    reserve_materialization(Program& program, Choice&& choice, PreparedPrompt&& prompt,
                            CancellationFlagView cancellation) {
        if (!std::holds_alternative<std::monostate>(transaction_) ||
            program.has_context_transaction()) {
            throw std::logic_error("ResourceManager already owns a resource transaction");
        }
        const ProgramResourceRevision resource_revision = program.resource_revision();
        if (!choice.plan_ || resource_revision.value == 0) {
            throw std::logic_error("resource choice is malformed");
        }
        if (choice.plan_->resource_revision() != resource_revision) {
            return MaterializationReserveResult::Stale;
        }
        validate_choice(choice, resource_revision);
        MaterializationRecord record = take_materialization_record(choice);
        transaction_.template emplace<MaterializationRecord>(std::move(record));
        MaterializationRecord& open = std::get<MaterializationRecord>(transaction_);
        reserve_logical_materialization(open);

        const ContextTransactionReserveStatus status = program.start_resource_transaction(
            std::move(*choice.plan_), std::move(prompt), cancellation);
        choice.plan_.reset();
        if (status == ContextTransactionReserveStatus::Aborted) {
            rollback_logical_materialization(open);
            transaction_.template emplace<std::monostate>();
            return cancellation.requested() ? MaterializationReserveResult::Aborted
                                            : MaterializationReserveResult::Stale;
        }
        observe_planner_diagnostics(open.diagnostics);
        return MaterializationReserveResult::Reserved;
    }

    [[nodiscard]] std::optional<ContextTransactionKind> context_transaction_kind() const noexcept {
        if (std::holds_alternative<MaterializationRecord>(transaction_)) {
            return ContextTransactionKind::Materialization;
        }
        if (std::holds_alternative<ActiveCaptureRecord>(transaction_)) {
            return ContextTransactionKind::ActiveCapture;
        }
        return std::nullopt;
    }

    [[nodiscard]] ContextTransactionOutcome
    progress_context_transaction(Program& program, CancellationFlagView cancellation) {
        if (std::holds_alternative<std::monostate>(transaction_) ||
            !program.has_context_transaction()) {
            throw std::logic_error("ResourceManager has no progressable resource transaction");
        }
        ProgramContextTransactionProgress progress =
            program.progress_context_transaction(cancellation);
        return std::visit(
            [&](auto&& result) -> ContextTransactionOutcome {
                using Result = std::decay_t<decltype(result)>;
                if constexpr (std::is_same_v<Result, ContextTransactionInProgress>) {
                    return ContextTransactionInProgress{};
                } else if constexpr (std::is_same_v<Result, ProgramMaterializationResult>) {
                    return adopt_materialization_progress(program, std::move(result));
                } else if constexpr (std::is_same_v<Result, ProgramActiveCaptureResult>) {
                    return adopt_active_capture_progress(program, std::move(result));
                } else {
                    throw std::logic_error("Program returned an unsupported resource operation");
                }
            },
            std::move(progress));
    }

    void adopt(Program& program, PublishedActivation&& activation) noexcept {
        if (activation.owner_ != this || !activation.result_ ||
            activation.destination_.value >= lane_count_ ||
            lanes_[activation.destination_.value] != LogicalLaneState::Materializing ||
            !active_[activation.destination_.value].occupied ||
            !std::holds_alternative<MaterializationRecord>(transaction_) ||
            !program.has_context_transaction()) {
            std::terminate();
        }
        lanes_[activation.destination_.value] = LogicalLaneState::Active;
        activation.result_.reset();
        activation.owner_ = nullptr;
        transaction_.template emplace<std::monostate>();
        program.finalize_context_transaction();
    }

    [[nodiscard]] ActiveCaptureReserveResult
    reserve_active_capture(Program& program, LaneId lane, CaptureOffer&& offer,
                           std::uint32_t blocked_runnable_requests,
                           CancellationFlagView cancellation) {
        require_lane(lane, LogicalLaneState::Active);
        const bool manager_transaction = !std::holds_alternative<std::monostate>(transaction_);
        const bool program_transaction = program.has_context_transaction();
        if (manager_transaction != program_transaction) {
            throw std::logic_error("capture observes inconsistent transaction ownership");
        }
        if (program_transaction) {
            program.skip_capture(std::move(offer));
            return ActiveCaptureReserveResult::Skipped;
        }
        rebuild_prefix_index();

        CaptureAssessment private_baseline =
            program.inspect_capture(offer, nullptr, nullptr, std::nullopt, false, "baseline");
        std::optional<CheckpointRef> private_replacement;
        if (!private_baseline.private_replacement_candidates.empty()) {
            private_replacement =
                *std::min_element(private_baseline.private_replacement_candidates.begin(),
                                  private_baseline.private_replacement_candidates.end(),
                                  [](CheckpointRef lhs, CheckpointRef rhs) {
                                      return std::tuple{lhs.kind, lhs.frontier, lhs.ordinal} <
                                             std::tuple{rhs.kind, rhs.frontier, rhs.ordinal};
                                  });
            private_baseline =
                program.inspect_capture(offer, nullptr, nullptr, private_replacement, false, "candidate");
        }

        CaptureAssessment candidate =
            program.inspect_capture(offer, nullptr, nullptr, private_replacement, true, "candidate-shared");
        const SharedPrefixHandle* exact_shared = nullptr;
        if (candidate.publishes_shared) {
            for (const PrefixIndexEntry& index : prefix_index_) {
                if (!index.shared || !valid_prefix_index_entry(index) ||
                    index.key != candidate.shortlist_key) {
                    continue;
                }
                SharedCatalogEntry& entry = shared_catalog_[index.slot];
                if (program.shared_capture_matches(offer, *entry.handle)) {
                    exact_shared = &*entry.handle;
                    break;
                }
            }
        }
        if (exact_shared != nullptr) {
            if (!private_baseline.publishes_private || !private_baseline.physically_feasible) {
                program.skip_capture(std::move(offer));
                return ActiveCaptureReserveResult::Skipped;
            }
            transaction_.template emplace<ActiveCaptureRecord>(ActiveCaptureRecord{
                .lane              = lane,
                .publishes_private = true,
            });
            const ContextTransactionReserveStatus reserved = program.reserve_active_capture(
                std::move(offer), exact_shared, nullptr, private_replacement, false, cancellation);
            if (reserved == ContextTransactionReserveStatus::Aborted) {
                transaction_.template emplace<std::monostate>();
                return ActiveCaptureReserveResult::Skipped;
            }
            return ActiveCaptureReserveResult::Reserved;
        }

        struct CaptureScenario {
            CaptureAssessment assessment;
            std::uint32_t publication_slot        = kInvalidCatalogSlot;
            const SharedPrefixHandle* replacement = nullptr;
            std::uint64_t replacement_id          = 0;
            std::uint64_t replacement_revision    = 0;
            std::uint32_t stable_ordinal          = 0;
        };

        struct SelectedCapture {
            CaptureScenario scenario;
            typename CapturePlanner::Result plan;
        };

        std::optional<SelectedCapture> selected;
        std::vector<PlanningOwnerRecord> capture_owner_records;
        if (candidate.publishes_shared) {
            const bool pressure_evidence =
                has_shared_candidate_evidence(candidate.shared_evidence,
                                              SharedCandidateEvidence::ExplicitBoundary) ||
                has_shared_candidate_evidence(candidate.shared_evidence,
                                              SharedCandidateEvidence::RequestedAutomatic) ||
                matching_reuse_domains(candidate.shortlist_key) >= 2U;

            std::vector<CaptureScenario> scenarios;
            scenarios.reserve(static_cast<std::size_t>(shared_catalog_count_) + 1U);
            for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                if (shared_catalog_[slot].state != SharedCatalogState::Vacant) { continue; }
                scenarios.push_back(CaptureScenario{
                    .assessment       = candidate,
                    .publication_slot = slot,
                    .stable_ordinal   = 0,
                });
                break;
            }
            if (pressure_evidence) {
                for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                    SharedCatalogEntry& entry = shared_catalog_[slot];
                    if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                        entry.transaction_pins != 0 || shared_active_edge_count(slot) != 0) {
                        continue;
                    }
                    CaptureAssessment assessment = program.inspect_capture(
                        offer, nullptr, &*entry.handle, private_replacement, true, "shared-replacement");
                    if (!assessment.publishes_shared) { continue; }
                    scenarios.push_back(CaptureScenario{
                        .assessment           = std::move(assessment),
                        .publication_slot     = slot,
                        .replacement          = &*entry.handle,
                        .replacement_id       = entry.id,
                        .replacement_revision = entry.revision,
                        .stable_ordinal       = 1U + slot,
                    });
                }
            }

            std::vector<typename CapturePlanner::OwnerPolicy> owner_policies;
            std::vector<typename CapturePlanner::CheckpointPolicy> checkpoint_policies;
            owner_policies.reserve(catalog_count_ + shared_catalog_count_);
            checkpoint_policies.reserve(prefix_index_.size());
            capture_owner_records.reserve(catalog_count_ + shared_catalog_count_);
            const auto append_private_checkpoint = [&](PlanningOwnerId owner, std::uint32_t slot,
                                                       const auto& checkpoint) {
                const CatalogEntry& entry = catalog_[slot];
                checkpoint_policies.push_back(typename CapturePlanner::CheckpointPolicy{
                    .owner                = owner,
                    .checkpoint           = checkpoint.ref,
                    .demand_mask          = committed_demand_mask_for(checkpoint.shortlist_key),
                    .rebuild_ns           = cost_model_.prefill_ns(checkpoint.rebuild_work),
                    .baseline_recovery_ns = price_checkpoint_recovery_work(
                        cost_model_,
                        program.checkpoint_recovery_work(*entry.handle, checkpoint.ref)),
                });
            };
            for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
                const CatalogEntry& entry = catalog_[slot];
                if (entry.state != CatalogState::Catalogued || !entry.handle ||
                    private_has_active_edge(slot)) {
                    continue;
                }
                const PlanningOwnerId owner{
                    .value = static_cast<std::uint32_t>(capture_owner_records.size())};
                capture_owner_records.push_back(PlanningOwnerRecord{
                    .id = owner,
                    .capability =
                        CatalogCapability{
                            .owner =
                                LogicalOwnerKey{
                                    .kind = LogicalOwnerKind::PrivateContinuation,
                                    .id   = entry.id,
                                },
                            .slot       = slot,
                            .generation = entry.revision,
                        },
                });
                owner_policies.push_back(typename CapturePlanner::OwnerPolicy{
                    .owner                    = owner,
                    .private_retention_weight = private_retention_weight(entry.retention),
                });
                if (entry.summary.endpoint) {
                    append_private_checkpoint(owner, slot, *entry.summary.endpoint);
                }
                if (entry.summary.rewrite) {
                    append_private_checkpoint(owner, slot, *entry.summary.rewrite);
                }
                for (const auto& checkpoint : entry.summary.long_anchors) {
                    append_private_checkpoint(owner, slot, checkpoint);
                }
            }
            for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                const SharedCatalogEntry& entry = shared_catalog_[slot];
                if (entry.state != SharedCatalogState::Catalogued || !entry.handle) { continue; }
                const PlanningOwnerId owner{
                    .value = static_cast<std::uint32_t>(capture_owner_records.size())};
                capture_owner_records.push_back(PlanningOwnerRecord{
                    .id = owner,
                    .capability =
                        CatalogCapability{
                            .owner =
                                LogicalOwnerKey{
                                    .kind = LogicalOwnerKind::SharedPrefix,
                                    .id   = entry.id,
                                },
                            .slot       = slot,
                            .generation = entry.revision,
                        },
                });
                owner_policies.push_back(typename CapturePlanner::OwnerPolicy{
                    .owner                    = owner,
                    .private_retention_weight = 0,
                    .explicit_shared_credit   = entry.explicit_credit,
                });
                checkpoint_policies.push_back(typename CapturePlanner::CheckpointPolicy{
                    .owner      = owner,
                    .checkpoint = entry.summary.checkpoint.ref,
                    .demand_mask =
                        committed_demand_mask_for(entry.summary.checkpoint.shortlist_key),
                    .rebuild_ns = cost_model_.prefill_ns(entry.summary.checkpoint.rebuild_work),
                    .baseline_recovery_ns = price_checkpoint_recovery_work(
                        cost_model_, program.checkpoint_recovery_work(
                                         *entry.handle, entry.summary.checkpoint.ref)),
                });
            }

            const std::uint32_t scenario_budget =
                scenarios.empty()
                    ? 0
                    : std::max<std::uint32_t>(1U, CapturePlanner::kTargetBudget /
                                                      static_cast<std::uint32_t>(scenarios.size()));
            for (CaptureScenario& scenario : scenarios) {
                std::vector<const ContinuationHandle*> private_owners;
                std::vector<PlanningOwnerId> private_owner_ids;
                std::vector<const SharedPrefixHandle*> shared_owners;
                std::vector<PlanningOwnerId> shared_owner_ids;
                const auto owner_id_for = [&](LogicalOwnerKind kind,
                                              std::uint32_t slot) -> PlanningOwnerId {
                    const auto found =
                        std::find_if(capture_owner_records.begin(), capture_owner_records.end(),
                                     [&](const auto& record) {
                                         return record.capability.owner.kind == kind &&
                                                record.capability.slot == slot;
                                     });
                    if (found == capture_owner_records.end()) {
                        throw std::logic_error("capture owner has no planning ID");
                    }
                    return found->id;
                };
                if (pressure_evidence) {
                    private_owners.reserve(catalog_count_);
                    private_owner_ids.reserve(catalog_count_);
                    shared_owners.reserve(shared_catalog_count_);
                    shared_owner_ids.reserve(shared_catalog_count_);
                    for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
                        const CatalogEntry& entry = catalog_[slot];
                        if (entry.state != CatalogState::Catalogued || !entry.handle ||
                            private_has_active_edge(slot)) {
                            continue;
                        }
                        private_owners.push_back(&*entry.handle);
                        private_owner_ids.push_back(
                            owner_id_for(LogicalOwnerKind::PrivateContinuation, slot));
                    }
                    for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                        const SharedCatalogEntry& entry = shared_catalog_[slot];
                        if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                            entry.transaction_pins != 0 || shared_active_edge_count(slot) != 0 ||
                            entry.id == scenario.replacement_id) {
                            continue;
                        }
                        shared_owners.push_back(&*entry.handle);
                        shared_owner_ids.push_back(
                            owner_id_for(LogicalOwnerKind::SharedPrefix, slot));
                    }
                }
                const typename CapturePlanner::Input input{
                    .capture             = &scenario.assessment,
                    .private_owners      = private_owners,
                    .private_owner_ids   = private_owner_ids,
                    .shared_owners       = shared_owners,
                    .shared_owner_ids    = shared_owner_ids,
                    .owner_policies      = owner_policies,
                    .checkpoint_policies = checkpoint_policies,
                    .direct_shared_victim =
                        scenario.replacement == nullptr
                            ? std::nullopt
                            : std::optional<PlanningOwnerId>(owner_id_for(
                                  LogicalOwnerKind::SharedPrefix, scenario.publication_slot)),
                    .candidate_demand_mask =
                        committed_demand_mask_for(scenario.assessment.shortlist_key),
                    .candidate_rebuild_ns =
                        cost_model_.prefill_ns(scenario.assessment.protected_rebuild_work),
                    .private_baseline_immediate_ns = price_context_transfer_requirements(
                        cost_model_, private_baseline.transfer_requirements),
                    .blocked_runnable_requests = blocked_runnable_requests,
                    .stable_scenario_ordinal   = scenario.stable_ordinal,
                    .target_budget             = scenario_budget,
                };
                std::optional<typename CapturePlanner::Result> planned =
                    capture_planner_.plan(program, cost_model_, input);
                if (!planned) { continue; }
                const bool better =
                    !selected || planned->net_gain > selected->plan.net_gain ||
                    (planned->net_gain == selected->plan.net_gain &&
                     std::tie(planned->stable_scenario_ordinal, planned->stable_target_ordinal) <
                         std::tie(selected->plan.stable_scenario_ordinal,
                                  selected->plan.stable_target_ordinal));
                if (better) {
                    selected.emplace(SelectedCapture{
                        .scenario = std::move(scenario),
                        .plan     = std::move(*planned),
                    });
                }
            }
        }

        if (!selected) {
            if (!private_baseline.publishes_private || !private_baseline.physically_feasible) {
                program.skip_capture(std::move(offer));
                return ActiveCaptureReserveResult::Skipped;
            }
            transaction_.template emplace<ActiveCaptureRecord>(ActiveCaptureRecord{
                .lane              = lane,
                .publishes_private = true,
            });
            const ContextTransactionReserveStatus reserved = program.reserve_active_capture(
                std::move(offer), nullptr, nullptr, private_replacement, false, cancellation);
            if (reserved == ContextTransactionReserveStatus::Aborted) {
                transaction_.template emplace<std::monostate>();
                return ActiveCaptureReserveResult::Skipped;
            }
            return ActiveCaptureReserveResult::Reserved;
        }

        ActiveCaptureRecord record{
            .lane                 = lane,
            .publishes_private    = selected->scenario.assessment.publishes_private,
            .publishes_shared     = true,
            .publication_slot     = selected->scenario.publication_slot,
            .replacement_id       = selected->scenario.replacement_id,
            .replacement_revision = selected->scenario.replacement_revision,
            .shared_evidence      = selected->scenario.assessment.shared_evidence,
        };
        for (const PressureOwnerOutcome& outcome : selected->plan.owner_outcomes) {
            const auto owner_record =
                std::find_if(capture_owner_records.begin(), capture_owner_records.end(),
                             [&](const PlanningOwnerRecord& candidate) {
                                 return candidate.id == outcome.owner;
                             });
            if (owner_record == capture_owner_records.end()) {
                throw std::logic_error("shared capture pressure owner ID is invalid");
            }
            const bool shared =
                owner_record->capability.owner.kind == LogicalOwnerKind::SharedPrefix;
            const std::uint32_t slot = owner_record->capability.slot;
            if (!shared) {
                if (slot >= catalog_count_) {
                    throw std::logic_error("shared capture private pressure owner is invalid");
                }
                const CatalogEntry& entry = catalog_[slot];
                if (entry.state != CatalogState::Catalogued || !entry.handle ||
                    entry.id != owner_record->capability.owner.id ||
                    entry.revision != owner_record->capability.generation ||
                    private_has_active_edge(slot)) {
                    throw std::logic_error("shared capture private pressure owner is stale");
                }
                std::vector<CheckpointRef> dropped = selected_checkpoint_drops(
                    outcome.owner, outcome.disposition, outcome.dropped_checkpoints,
                    selected->plan.checkpoint_outcomes,
                    continuation_checkpoint_count(entry.summary), [&](CheckpointRef checkpoint) {
                        return continuation_contains_checkpoint(entry.summary, checkpoint);
                    });
                record.private_claims.push_back(OwnerClaim{
                    .planning_id         = outcome.owner,
                    .capability          = owner_record->capability,
                    .disposition         = outcome.disposition,
                    .dropped_checkpoints = std::move(dropped),
                });
            } else {
                if (slot >= shared_catalog_count_ || slot == record.publication_slot) {
                    throw std::logic_error("shared capture shared pressure owner is duplicated");
                }
                const SharedCatalogEntry& entry = shared_catalog_[slot];
                if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                    entry.id != owner_record->capability.owner.id ||
                    entry.revision != owner_record->capability.generation ||
                    entry.transaction_pins != 0 || shared_active_edge_count(slot) != 0) {
                    throw std::logic_error("shared capture shared pressure owner is stale");
                }
                std::vector<CheckpointRef> dropped = selected_checkpoint_drops(
                    outcome.owner, outcome.disposition, outcome.dropped_checkpoints,
                    selected->plan.checkpoint_outcomes, 1U, [&](CheckpointRef checkpoint) {
                        return checkpoint == entry.summary.checkpoint.ref;
                    });
                record.shared_claims.push_back(OwnerClaim{
                    .planning_id         = outcome.owner,
                    .capability          = owner_record->capability,
                    .disposition         = outcome.disposition,
                    .dropped_checkpoints = std::move(dropped),
                });
            }
        }
        if (!selected->plan.pressure) {
            throw std::logic_error("selected shared capture has no pressure plan");
        }
        if (record.publication_slot >= shared_catalog_count_) {
            throw std::logic_error("selected shared publication slot is invalid");
        }
        const SharedCatalogEntry& publication = shared_catalog_[record.publication_slot];
        if (record.replacement_id == 0) {
            if (publication.state != SharedCatalogState::Vacant || publication.id != 0 ||
                publication.handle || publication.transaction_pins != 0 ||
                shared_active_edge_count(record.publication_slot) != 0) {
                throw std::logic_error("selected vacant shared publication slot changed");
            }
        } else if (publication.state != SharedCatalogState::Catalogued || !publication.handle ||
                   publication.id != record.replacement_id ||
                   publication.revision != record.replacement_revision ||
                   publication.transaction_pins != 0 ||
                   shared_active_edge_count(record.publication_slot) != 0) {
            throw std::logic_error("selected shared replacement changed before reservation");
        }

        transaction_.template emplace<ActiveCaptureRecord>(std::move(record));
        ActiveCaptureRecord& open = std::get<ActiveCaptureRecord>(transaction_);
        reserve_logical_active_capture(open);
        const ContextTransactionReserveStatus reserved =
            program.reserve_active_capture_with_pressure(
                std::move(offer), nullptr, selected->scenario.replacement, private_replacement,
                true, std::move(*selected->plan.pressure), cancellation);
        if (reserved == ContextTransactionReserveStatus::Aborted) {
            rollback_logical_active_capture(open);
            transaction_.template emplace<std::monostate>();
            return ActiveCaptureReserveResult::Skipped;
        }
        return ActiveCaptureReserveResult::Reserved;
    }

    void mark_terminal_pending(LaneId lane) {
        require_lane(lane, LogicalLaneState::Active);
        lanes_[lane.value] = LogicalLaneState::TerminalPending;
    }

    [[nodiscard]] FinishResult finish(Program& program, LaneId lane, SequenceHandle sequence) {
        require_lane(lane, LogicalLaneState::TerminalPending);
        if (!std::holds_alternative<std::monostate>(transaction_) ||
            program.has_context_transaction()) {
            throw std::logic_error("terminal finish overlaps an open resource transaction");
        }
        ActiveEntry& active = active_[lane.value];
        FinishResult result = program.finish(sequence);
        if (result.status != ConsumeStatus::Consumed) {
            AbortResult discarded = program.abort(sequence);
            if (discarded.status != ConsumeStatus::Consumed) {
                throw std::logic_error(
                    "Program could neither retain nor discard terminal sequence");
            }
            release_active_references(lane);
            note_cell_clear(CellClearReason::Terminal);
            clear_catalog_entry(catalog_.at(active.publication_slot));
            reset_active_entry(active);
            lanes_[lane.value] = LogicalLaneState::Free;

            FinishResult released;
            released.status      = ConsumeStatus::Consumed;
            released.disposition = FinishDisposition::Released;
            released.timings     = discarded.timings;
            released.speculative = std::move(discarded.speculative);
            return released;
        }
        CatalogEntry& publication = catalog_.at(active.publication_slot);
        if (!cache_enabled_ || result.disposition == FinishDisposition::Released) {
            if (result.disposition != FinishDisposition::Released || result.continuation) {
                throw std::logic_error("released finish returned a continuation");
            }
            release_active_references(lane);
            note_cell_clear(CellClearReason::Terminal);
            clear_catalog_entry(publication);
            reset_active_entry(active);
            lanes_[lane.value] = LogicalLaneState::Free;
            return result;
        }
        if (result.disposition != FinishDisposition::Catalogued || !result.continuation ||
            !valid_continuation_summary(result.summary) ||
            publication.state != CatalogState::ReservedForActive ||
            publication.id != active.continuation_id) {
            if (result.continuation) {
                (void)program.release_continuation(std::move(*result.continuation));
                result.continuation.reset();
            }
            throw std::logic_error("Program returned an invalid terminal continuation");
        }

        release_active_references(lane);
        publication.state = CatalogState::Catalogued;
        assign_continuation_summary(publication.summary, result.summary);
        publication.handle.emplace(std::move(*result.continuation));
        result.continuation.reset();
        publication.session   = active.session;
        publication.retention = active.retention;
        migrate_observations(publication, result.summary, active.retention);
        advance_revision(publication.revision);
        if (publication.session && active.update_session_index) {
            if (!publish_session(*publication.session, active.publication_slot, publication.id,
                                 publication.revision, active.publication_order)) {
                publication.session.reset();
                publication.retention = RetentionClass::RecentPrivate;
            }
        }
        reset_active_entry(active);
        lanes_[lane.value] = LogicalLaneState::Free;
        return result;
    }

    [[nodiscard]] AbortResult abort(Program& program, LaneId lane, SequenceHandle sequence) {
        if (!std::holds_alternative<std::monostate>(transaction_) ||
            program.has_context_transaction()) {
            throw std::logic_error("terminal abort overlaps an open resource transaction");
        }
        if (lanes_.at(lane.value) == LogicalLaneState::Active) {
            lanes_[lane.value] = LogicalLaneState::TerminalPending;
        }
        require_lane(lane, LogicalLaneState::TerminalPending);
        AbortResult result = program.abort(sequence);
        if (result.status != ConsumeStatus::Consumed) {
            throw std::logic_error("Program did not consume aborted sequence");
        }
        release_active_references(lane);
        note_cell_clear(CellClearReason::Cancelled);
    clear_catalog_entry(catalog_.at(active_[lane.value].publication_slot));
        reset_active_entry(active_[lane.value]);
        lanes_[lane.value] = LogicalLaneState::Free;
        return result;
    }

    void apply_commit(std::span<const LaneId> lanes,
                      const typename ModelContract::CommitResult& result) {
        if (lanes.size() != result.row_count) {
            throw std::logic_error("commit result membership is not row aligned");
        }
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            const LaneId lane = lanes[row];
            require_lane(lane, LogicalLaneState::Active);
            switch (result.rows[row].disposition) {
            case CommitDisposition::Active:
                break;
            case CommitDisposition::Finishable:
                lanes_[lane.value] = LogicalLaneState::TerminalPending;
                break;
            case CommitDisposition::CancelledReleased:
                release_cancelled_lane(lane);
                break;
            }
        }
    }

    void apply_discard(std::span<const LaneId> lanes,
                       const typename ModelContract::DiscardResult& result) {
        if (lanes.size() != result.row_count || result.status != ConsumeStatus::Consumed) {
            throw std::logic_error("pending discard did not consume its membership");
        }
        for (const LaneId lane : lanes) { release_cancelled_lane(lane); }
    }

    void release_failed_commit(std::span<const LaneId> lanes) noexcept {
        for (const LaneId lane : lanes) {
            if (lane.value < lane_count_ && active_[lane.value].occupied) {
                try {
                    release_cancelled_lane(lane);
                } catch (...) {}
            }
        }
    }

    // Monotonic process-wide transfer totals, split by direction.  Callers take a baseline
    // and subtract; see ContextTransferTotals for why the split is by direction and not by
    // resource class.
    [[nodiscard]] ContextTransferTotals transfer_totals() const noexcept {
        return transfer_totals_;
    }

    void populate_runtime_stats(Program& program, RuntimeStats& out) const noexcept {
        out.state_moves                        = context_stats_.state_moves;
        out.state_forks                        = context_stats_.state_forks;
        out.state_restores                     = context_stats_.state_restores;
        out.state_d2h_count                    = context_stats_.state_d2h_count;
        out.state_h2d_count                    = context_stats_.state_h2d_count;
        out.state_d2d_count                    = context_stats_.state_d2d_count;
        out.state_d2h_bytes                    = context_stats_.state_d2h_bytes;
        out.state_h2d_bytes                    = context_stats_.state_h2d_bytes;
        out.state_d2d_bytes                    = context_stats_.state_d2d_bytes;
        out.state_d2h_seconds                  = context_stats_.state_d2h_seconds;
        out.state_h2d_seconds                  = context_stats_.state_h2d_seconds;
        out.state_d2d_seconds                  = context_stats_.state_d2d_seconds;
        out.main_kv_d2h_pages                  = context_stats_.main_kv_d2h_pages;
        out.main_kv_h2d_pages                  = context_stats_.main_kv_h2d_pages;
        out.main_kv_d2d_pages                  = context_stats_.main_kv_d2d_pages;
        out.main_kv_d2h_bytes                  = context_stats_.main_kv_d2h_bytes;
        out.main_kv_h2d_bytes                  = context_stats_.main_kv_h2d_bytes;
        out.main_kv_d2d_bytes                  = context_stats_.main_kv_d2d_bytes;
        out.main_kv_d2h_seconds                = context_stats_.main_kv_d2h_seconds;
        out.main_kv_h2d_seconds                = context_stats_.main_kv_h2d_seconds;
        out.main_kv_d2d_seconds                = context_stats_.main_kv_d2d_seconds;
        out.backend_kv_d2h_pages               = context_stats_.backend_kv_d2h_pages;
        out.backend_kv_h2d_pages               = context_stats_.backend_kv_h2d_pages;
        out.backend_kv_d2d_pages               = context_stats_.backend_kv_d2d_pages;
        out.backend_kv_d2h_bytes               = context_stats_.backend_kv_d2h_bytes;
        out.backend_kv_h2d_bytes               = context_stats_.backend_kv_h2d_bytes;
        out.backend_kv_d2d_bytes               = context_stats_.backend_kv_d2d_bytes;
        out.backend_kv_d2h_seconds             = context_stats_.backend_kv_d2h_seconds;
        out.backend_kv_h2d_seconds             = context_stats_.backend_kv_h2d_seconds;
        out.backend_kv_d2d_seconds             = context_stats_.backend_kv_d2d_seconds;
        out.pressure_spill_pages               = context_stats_.pressure_spill_pages;
        out.partial_tail_cow_pages             = context_stats_.partial_tail_cow_pages;
        out.pressure_private_owners_degraded   = context_stats_.pressure_private_owners_degraded;
        out.pressure_private_owners_demoted    = context_stats_.pressure_private_owners_demoted;
        out.pressure_private_owners_demoted_kv      = context_stats_.pressure_private_owners_demoted_kv;
        out.pressure_private_owners_demoted_kv_only = context_stats_.pressure_private_owners_demoted_kv_only;
        out.pressure_private_owners_evicted    = context_stats_.pressure_private_owners_evicted;
        out.pressure_shared_owners_degraded    = context_stats_.pressure_shared_owners_degraded;
        out.pressure_shared_owners_evicted     = context_stats_.pressure_shared_owners_evicted;
        out.pressure_shared_owners_replaced    = program.shared_replacements();
        out.pressure_private_evictions_demotable = program.demotable_evictions();
        out.pressure_evictions_with_victim_room  = program.evictions_with_victim_room();
        out.pressure_private_eviction_checks    = program.demotable_eviction_checks();
        out.pressure_demote_options             = program.demote_options();
        out.pressure_options                    = program.pressure_options();
        // WIRED WITH THE COUNTERS, not after them: the catalog-cell counters shipped incremented at nine sites
        // and never copied, so /stats served a hardcoded zero that read like a clean finding.
        out.capture_skips_transaction_or_fork      = program.capture_skips(0U);
        out.capture_skips_cancelled                = program.capture_skips(1U);
        out.capture_skips_nothing_to_publish       = program.capture_skips(2U);
        out.capture_skips_stale_pressure_plan      = program.capture_skips(3U);
        out.capture_skips_not_feasible_no_pressure = program.capture_skips(4U);
        // The catalog's capacity/occupancy pair, as of this publication. It had neither half, which is why its exhaustion could
        // only be noticed as a silent loss of reuse. (Not the only pool in that state: the shared-prefix pool
        // has neither half either and device-state-slots has occupancy without a capacity -- `plan.md` §4.)
        out.private_catalog_capacity_cells      = catalog_count_;
        // THE COPY-OUT, and its absence is why the first reading of these five was all zeros: the counters
        // were incremented at nine sites and never copied, so `/stats` served the struct's default 0 -- a dead
        // instrument reading as a clean zero, which would have been read as "no cell is ever cleared" and that
        // is a claim about the ENGINE drawn from a counter that was never wired. `terminal` alone must be at
        // least one per completed request, so a zero here is an instrument failure, not a finding.
        out.catalog_cell_clears_terminal  = cell_clears_[static_cast<std::size_t>(CellClearReason::Terminal)];
        out.catalog_cell_clears_action    = cell_clears_[static_cast<std::size_t>(CellClearReason::Action)];
        out.catalog_cell_clears_cancelled = cell_clears_[static_cast<std::size_t>(CellClearReason::Cancelled)];
        out.catalog_cell_clears_cleanup   = cell_clears_[static_cast<std::size_t>(CellClearReason::Cleanup)];
        out.catalog_cell_clears_rollback  = cell_clears_[static_cast<std::size_t>(CellClearReason::Rollback)];
        out.session_erasures_eviction = session_erases_[static_cast<std::size_t>(SessionEraseReason::Eviction)];
        out.session_erasures_consume  = session_erases_[static_cast<std::size_t>(SessionEraseReason::Consume)];
        // The session index's own capacity/occupancy pair. It had NEITHER, which is the same blindness the
        // private catalog had before it got one: `session_cell_frontier == 0` says "no cell" without saying
        // whether the index is full, empty, or the entry was erased.
        out.session_index_capacity_cells = static_cast<std::uint32_t>(session_index_.size());
        out.session_index_occupied_cells = static_cast<std::uint32_t>(
            std::count_if(session_index_.begin(), session_index_.end(),
                          [](const SessionIndexEntry& e) { return e.state == SessionIndexState::Occupied; }));
        out.private_catalog_occupied_cells      = catalog_occupied_cells();
        // HOW MUCH OF THE SCARCE AXIS IS HELD BY CONTINUATIONS NOTHING IS ANCHORED TO. "Unanchored" is the
        // whole of the claim and it is NOT "reclaimable": candidate scans run over the entire catalog, so an
        // unanchored entry is still matchable -- measured, 76 request-log records reused a private entry that
        // was not their own session cell. So this counts what is not pinned by a cell or an edge, which is the
        // denominator a reclaim policy would have to reason about, not a population it could free.
        //
        // The forced evictions of
        // 2026-09-28 were all `demote_refusal=already-on-host` with `victim_host_slots=2..3`: the plan was
        // short on HOST STATE SLOTS and the only way to close a host-state deficit is to drop something that
        // HOLDS host state -- a demote moves state onto the scarce axis, it cannot relieve it. So before any
        // reclaim policy can be aimed, the population has to exist and be countable: per catalogued
        // continuation, how many of its checkpoints sit on host (`CheckpointSummary.state_residency`, which
        // the store maintains at publish), split by whether anything can still reach it -- the session cell
        // this key resolves to, or an active lane edge. A checkpoint is a proxy for a slot, not a slot count:
        // the per-victim tallies showed 2-3 per victim (endpoint + rewrite + anchors), which is this number.
        {
            std::uint32_t reachable = 0;
            std::uint32_t orphaned  = 0;
            // Generic, because the checkpoint type is a ModelContract alias and naming it here would couple
            // this model-agnostic file to one model's names.
            const auto hosts = [](const auto& summary) -> std::uint32_t {
                const auto on_host = [](const auto& checkpoint) -> std::uint32_t {
                    return checkpoint.state_residency != ReplicaResidency::DeviceOnly ? 1U : 0U;
                };
                std::uint32_t n = 0;
                if (summary.endpoint) { n += on_host(*summary.endpoint); }
                if (summary.rewrite) { n += on_host(*summary.rewrite); }
                for (const auto& anchor : summary.long_anchors) { n += on_host(anchor); }
                return n;
            };
            for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
                const CatalogEntry& entry = catalog_[slot];
                if (entry.state != CatalogState::Catalogued || !entry.handle) { continue; }
                const std::uint32_t n = hosts(entry.summary);
                if (n == 0) { continue; }
                bool held = private_has_active_edge(slot);
                if (!held) {
                    for (std::size_t cell = 0; cell < session_index_.size(); ++cell) {
                        if (session_index_[cell].state == SessionIndexState::Occupied &&
                            session_index_[cell].slot == slot) { held = true; break; }
                    }
                }
                (held ? reachable : orphaned) += n;
            }
            out.host_state_checkpoints_reachable = reachable;
            out.host_state_checkpoints_unanchored  = orphaned;
        }
        out.pressure_publication_cell_losses    = program.publication_cell_losses();
        out.pressure_publication_cell_probes    = program.publication_cell_probes();
        out.pressure_publication_cell_at_risk_runs = program.publication_cell_at_risk_runs();
        out.pressure_goal_blocked_cell_only        = program.publication_goal_blocked_cell_only();
        out.pressure_goal_blocked_other            = program.publication_goal_blocked_other();
        out.pressure_publication_cell_veto_goals   = program.publication_cell_veto_goals();
        out.pressure_publication_cell_veto_other   = program.publication_cell_veto_other();
        out.pressure_publication_cell_veto_reuse   = program.publication_cell_veto_reuse();
        out.pressure_checkpoints_dropped       = context_stats_.pressure_checkpoints_dropped;
        out.pressure_searches                  = context_stats_.pressure_searches;
        out.pressure_search_budget_exhaustions = context_stats_.pressure_search_budget_exhaustions;
        out.pressure_target_arena_truncations  = context_stats_.pressure_target_arena_truncations;
        out.pressure_maximal_fallback_selections =
            context_stats_.pressure_maximal_fallback_selections;
        out.historical_fork_hits            = context_stats_.historical_fork_hits;
        out.actual_context_transfer_seconds = context_stats_.actual_context_transfer_seconds;

        const auto usage                     = program.physical_usage();
        out.device_state_occupied_slots      = usage.device_state_slots;
        out.host_state_occupied_slots        = usage.host_state_slots;
        out.device_main_kv_occupied_pages    = usage.device_main_kv_pages;
        out.device_backend_kv_occupied_pages = usage.device_backend_kv_pages;
        out.host_kv_occupied_bytes           = usage.host_kv_bytes;
        std::uint64_t shared_references      = 0;
        for (std::uint32_t lane = 0; lane < lane_count_; ++lane) {
            if (active_[lane].occupied) {
                shared_references += active_[lane].shared_sources.size();
            }
        }
        out.shared_active_references = shared_references > std::numeric_limits<std::uint32_t>::max()
                                           ? std::numeric_limits<std::uint32_t>::max()
                                           : static_cast<std::uint32_t>(shared_references);
    }

    [[nodiscard]] CatalogState catalog_state(std::uint32_t slot) const noexcept {
        return slot < catalog_count_ ? catalog_[slot].state : CatalogState::Vacant;
    }

    [[nodiscard]] LogicalLaneState lane_state(LaneId lane) const noexcept {
        return lane.value < lane_count_ ? lanes_[lane.value] : LogicalLaneState::Free;
    }

    void clear_after_program_cleanup() noexcept {
        transaction_.template emplace<std::monostate>();
        for (CatalogEntry& entry : catalog_) {
            // COUNTED ONLY WHEN THE SLOT HELD SOMETHING, and Action is NOT counted here at all.
            // This loop walks the whole array, so incrementing per slot made `cleanup` a count of the catalog
            // SIZE after any recovery rather than of cells that were cleared -- and it incremented `action`
            // too, for vacant slots, on top of the Evicted branch's own count. So `action` jumped by 64 on the
            // first recovery and `action == private_owners_evicted` (which held all day) would have broken
            // there without anything recording it. `action` now means one thing: a private owner evicted.
            const bool held = entry.state != CatalogState::Vacant || entry.handle.has_value();
            entry.handle.reset();
            if (held) { note_cell_clear(CellClearReason::Cleanup); }
            clear_catalog_entry(entry);
        }
        for (SharedCatalogEntry& entry : shared_catalog_) {
            entry.handle.reset();
            clear_shared_entry(entry);
        }
        for (SessionIndexEntry& entry : session_index_) { entry = {}; }
        for (std::uint32_t lane = 0; lane < lane_count_; ++lane) {
            lanes_[lane] = LogicalLaneState::Free;
            reset_active_entry(active_[lane]);
        }
        demand_window_.clear();
        demand_epoch_ = 0;
    }

private:
    // Counters the candidate loop produces and `plan_materialization` reports: they are known only where the
    // source was offered (the retain decision and the endpoint-beyond-prompt test), and read only where the
    // diagnostics are assembled, so they travel the same path as `candidates` rather than living as members.
    struct CandidateCounters {
        std::uint32_t sibling_candidates = 0;  // sources whose own endpoint lies beyond this prompt
        std::uint32_t retained_sources   = 0;
        std::uint32_t consumed_sources   = 0;
        // WHY THE CONVERSATION'S OWN CELL ENTRY WAS NOT A CANDIDATE. `session_cell_offered` says whether it
        // was; on the first real reading it was FALSE in 100% of shared-class requests -- including ones whose
        // cell held a deep frontier -- and a bare false cannot tell "no cell exists" from "the entry is held by
        // a running lane" from "identity refused it". The five sites below are where that is actually known.
        // PRIVATE ENTRIES ONLY -- a shared entry's slot is a different namespace (see `note_session_slot`).
        // COVERAGE: `test_resource_manager` pins codes 1, 3, 4, 5 and 8 by value -- each has a case whose
        // asserted number changes if its site is deleted. 9 is pinned only as an ABSENCE: a no-cell request
        // must NOT read it, and nothing asserts it is ever emitted (the table calls it unreachable).
        // 7 (a lane's active edge) is UNTESTED: it needs a second lane, and a mutant deleting that site
        // survives. Read 7 with that in mind.
        // 0 = no cell / no session key / `update_session_index` false / cache disabled; 1 = offered (the cell's
        // slot produced a candidate, and this is STICKY -- a later entry cannot undo it); 2 = the slot has no
        // index entry at all; 3 = no shortlist key at its frontier; 4 = identity-tag mismatch; 5 = digest
        // mismatch; 7 = the entry is held by an active lane's edge; 8 = `inspect_admission` refused it.
        // (6 is retired: the validator already requires `Catalogued && handle`. 9 -- "failed validation" -- is
        // emitted at that site but unreachable on the current path; kept rather than retired because a slot
        // recycled after the rebuild would land there, and that case is not audited.)
        std::uint8_t  session_cell_skip  = 0;
        // ...and what happened to the conversation's OWN endpoint specifically, which `session_cell_skip` cannot
        // report once a shallower anchor of the same slot has been offered. Same codes; 0 means the loop never
        // met an index entry that is this slot's endpoint.
        std::uint8_t  session_endpoint_skip = 0;
    };

    struct Candidate {
        std::optional<AdmissionCandidate> plan;
        bool current_session_binding = false;
        std::optional<CatalogCapability> private_source;
        std::optional<CatalogCapability> shared_source;
        std::optional<PolicyObservationKey> selected_observation;
        std::optional<PrefixShortlistKey> source_key;
    };

    struct CatalogEntry {
        CatalogState state     = CatalogState::Vacant;
        std::uint64_t id       = 0;
        std::uint64_t revision = 1;
        ContinuationSummary summary;
        std::optional<ContinuationHandle> handle;
        std::optional<CacheSessionKey> session;
        std::vector<CheckpointObservation> observations;
        RetentionClass retention = RetentionClass::RecentPrivate;
    };

    struct SharedCatalogEntry {
        SharedCatalogState state = SharedCatalogState::Vacant;
        std::uint64_t id         = 0;
        std::uint64_t revision   = 1;
        SharedPrefixSummary summary;
        std::optional<SharedPrefixHandle> handle;
        RetentionObservation observation{.retention_class = RetentionClass::SharedStable};
        std::uint32_t transaction_pins    = 0;
        bool explicit_credit              = false;
        std::uint64_t credit_expiry_epoch = 0;
    };

    enum class SessionIndexState : std::uint8_t {
        Empty,
        Occupied,
        Deleted,
    };

    struct SessionIndexEntry {
        SessionIndexState state = SessionIndexState::Empty;
        CacheSessionKey key;
        std::uint32_t slot              = kInvalidCatalogSlot;
        std::uint64_t owner_id          = 0;
        std::uint64_t revision          = 0;
        std::uint64_t publication_order = 0;
    };

    struct PrefixIndexEntry {
        bool occupied = false;
        bool shared   = false;
        PrefixShortlistKey key;
        std::uint32_t slot     = kInvalidCatalogSlot;
        std::uint64_t owner_id = 0;
        std::uint64_t revision = 0;
        CheckpointRef checkpoint;
    };

    struct ActiveEntry {
        bool occupied                  = false;
        std::uint32_t publication_slot = kInvalidCatalogSlot;
        std::uint64_t continuation_id  = 0;
        std::optional<CacheSessionKey> session;
        RetentionClass retention        = RetentionClass::RecentPrivate;
        bool update_session_index       = true;
        std::uint64_t publication_order = 0;
        std::optional<ActiveOwnerEdge> retained_private_source;
        std::vector<ActiveOwnerEdge> shared_sources;
    };

    struct MaterializationRecord {
        LaneId destination;
        std::optional<CatalogCapability> private_source;
        PrivateSourceMode source_mode = PrivateSourceMode::ConsumeToActive;
        std::optional<CatalogCapability> shared_source;
        std::uint32_t publication_slot = kInvalidCatalogSlot;
        std::vector<OwnerClaim> private_claims;
        std::vector<OwnerClaim> shared_claims;
        std::optional<PolicyObservationKey> selected_observation;
        std::optional<CacheSessionKey> session;
        RetentionClass retention        = RetentionClass::RecentPrivate;
        bool update_session_index       = true;
        std::uint64_t publication_order = 0;
        MaterializationDiagnostics diagnostics;
        PrefixDemandRecord demand;
    };

    struct ActiveCaptureRecord {
        LaneId lane;
        bool publishes_private                  = false;
        bool publishes_shared                   = false;
        std::uint32_t publication_slot          = kInvalidCatalogSlot;
        std::uint64_t replacement_id            = 0;
        std::uint64_t replacement_revision      = 0;
        SharedCandidateEvidence shared_evidence = SharedCandidateEvidence::None;
        std::vector<OwnerClaim> private_claims;
        std::vector<OwnerClaim> shared_claims;
    };

    [[nodiscard]] CatalogCapability private_capability(std::uint32_t slot) const {
        const CatalogEntry& entry = catalog_.at(slot);
        return CatalogCapability{
            .owner =
                LogicalOwnerKey{
                    .kind = LogicalOwnerKind::PrivateContinuation,
                    .id   = entry.id,
                },
            .slot       = slot,
            .generation = entry.revision,
        };
    }

    [[nodiscard]] CatalogCapability shared_capability(std::uint32_t slot) const {
        const SharedCatalogEntry& entry = shared_catalog_.at(slot);
        return CatalogCapability{
            .owner =
                LogicalOwnerKey{
                    .kind = LogicalOwnerKind::SharedPrefix,
                    .id   = entry.id,
                },
            .slot       = slot,
            .generation = entry.revision,
        };
    }

    [[nodiscard]] static constexpr ActiveOwnerEdge
    active_edge(CatalogCapability capability) noexcept {
        return ActiveOwnerEdge{.owner = capability.owner, .slot = capability.slot};
    }

    [[nodiscard]] bool private_has_active_edge(std::uint32_t slot) const noexcept {
        return std::any_of(active_.begin(), active_.begin() + lane_count_,
                           [&](const ActiveEntry& active) {
                               return active.occupied && active.retained_private_source &&
                                      active.retained_private_source->slot == slot;
                           });
    }

    [[nodiscard]] std::uint32_t shared_active_edge_count(std::uint32_t slot) const noexcept {
        std::uint32_t count = 0;
        for (std::uint32_t lane = 0; lane < lane_count_; ++lane) {
            const ActiveEntry& active = active_[lane];
            if (!active.occupied) { continue; }
            count += static_cast<std::uint32_t>(
                std::count_if(active.shared_sources.begin(), active.shared_sources.end(),
                              [&](const ActiveOwnerEdge& edge) { return edge.slot == slot; }));
        }
        return count;
    }

    void reset_active_entry(ActiveEntry& active) noexcept {
        active.occupied         = false;
        active.publication_slot = kInvalidCatalogSlot;
        active.continuation_id  = 0;
        active.session.reset();
        active.retention            = RetentionClass::RecentPrivate;
        active.update_session_index = true;
        active.publication_order    = 0;
        active.retained_private_source.reset();
        active.shared_sources.clear();
    }

    [[nodiscard]] static std::size_t checked_prefix_index_capacity(std::uint32_t private_capacity,
                                                                   std::uint32_t shared_capacity,
                                                                   std::uint32_t max_long_anchors) {
        const std::size_t width = static_cast<std::size_t>(max_long_anchors) + 2U;
        if (private_capacity != 0 &&
            width >
                (std::numeric_limits<std::size_t>::max() - shared_capacity) / private_capacity) {
            throw std::overflow_error("prefix index capacity overflow");
        }
        return static_cast<std::size_t>(private_capacity) * width + shared_capacity;
    }

    static void advance_revision(std::uint64_t& revision) noexcept {
        if (++revision == 0) { ++revision; }
    }

    static void saturating_increment(std::uint64_t& value) noexcept {
        if (value != std::numeric_limits<std::uint64_t>::max()) { ++value; }
    }

    static constexpr std::size_t kDemandWindowCapacity = 32U;

    static void append_unique(std::vector<PrefixShortlistKey>& destination,
                              const PrefixShortlistKey& key) {
        if (std::find(destination.begin(), destination.end(), key) == destination.end()) {
            destination.push_back(key);
        }
    }

    [[nodiscard]] static bool demand_matches(const PrefixDemandRecord& demand,
                                             const PrefixShortlistKey& key) noexcept {
        return std::find(demand.candidate_keys.begin(), demand.candidate_keys.end(), key) !=
                   demand.candidate_keys.end() ||
               std::find(demand.exact_resident_keys.begin(), demand.exact_resident_keys.end(),
                         key) != demand.exact_resident_keys.end() ||
               (demand.selected_source_key && *demand.selected_source_key == key);
    }

    [[nodiscard]] static ReuseDomainId reuse_domain(const std::optional<CacheSessionKey>& session,
                                                    std::uint64_t publication_order) noexcept {
        if (!session) {
            return ReuseDomainId{
                .low  = publication_order,
                .high = publication_order ^ 0xD6E8FEB86659FD93ULL,
            };
        }
        std::uint64_t low  = 1469598103934665603ULL;
        std::uint64_t high = 1099511628211ULL ^ 0x9E3779B97F4A7C15ULL;
        for (const unsigned char value : session->view()) {
            low ^= value;
            low *= 1099511628211ULL;
            high ^= static_cast<std::uint64_t>(value) + 0x9E3779B97F4A7C15ULL + (high << 6U) +
                    (high >> 2U);
            high *= 0xD6E8FEB86659FD93ULL;
        }
        return ReuseDomainId{.low = low, .high = high};
    }

    [[nodiscard]] std::uint32_t
    demand_mask_for(const PrefixShortlistKey& key,
                    const PrefixDemandRecord& provisional) const noexcept {
        std::uint32_t mask      = 0;
        std::uint32_t bit       = 0;
        const std::size_t begin = demand_window_.size() == kDemandWindowCapacity ? 1U : 0U;
        for (std::size_t index = begin; index < demand_window_.size(); ++index, ++bit) {
            if (demand_matches(demand_window_[index], key)) { mask |= 1U << bit; }
        }
        if (bit < kDemandWindowCapacity && demand_matches(provisional, key)) { mask |= 1U << bit; }
        return mask;
    }

    [[nodiscard]] std::uint32_t
    committed_demand_mask_for(const PrefixShortlistKey& key) const noexcept {
        std::uint32_t mask = 0;
        for (std::uint32_t bit = 0; bit < demand_window_.size(); ++bit) {
            if (demand_matches(demand_window_[bit], key)) { mask |= 1U << bit; }
        }
        return mask;
    }

    [[nodiscard]] std::size_t matching_reuse_domains(const PrefixShortlistKey& key) const noexcept {
        std::array<ReuseDomainId, kDemandWindowCapacity> domains{};
        std::size_t count = 0;
        for (const PrefixDemandRecord& demand : demand_window_) {
            if (!demand_matches(demand, key) ||
                std::find(domains.begin(), domains.begin() + static_cast<std::ptrdiff_t>(count),
                          demand.domain) != domains.begin() + static_cast<std::ptrdiff_t>(count)) {
                continue;
            }
            domains[count++] = demand.domain;
        }
        return count;
    }

    [[nodiscard]] std::size_t
    matching_reuse_domains(const PrefixShortlistKey& key,
                           const PrefixDemandRecord& provisional) const noexcept {
        std::array<ReuseDomainId, kDemandWindowCapacity> domains{};
        std::size_t count       = 0;
        const std::size_t begin = demand_window_.size() == kDemandWindowCapacity ? 1U : 0U;
        const auto append       = [&](const PrefixDemandRecord& demand) {
            if (!demand_matches(demand, key) ||
                std::find(domains.begin(), domains.begin() + static_cast<std::ptrdiff_t>(count),
                                demand.domain) != domains.begin() + static_cast<std::ptrdiff_t>(count)) {
                return;
            }
            domains[count++] = demand.domain;
        };
        for (std::size_t index = begin; index < demand_window_.size(); ++index) {
            append(demand_window_[index]);
        }
        append(provisional);
        return count;
    }

    [[nodiscard]] static std::uint32_t private_retention_weight(RetentionClass retention) noexcept {
        switch (retention) {
        case RetentionClass::Disposable:
            return 1;
        case RetentionClass::RecentPrivate:
            return 4;
        case RetentionClass::LiveSession:
            return 16;
        case RetentionClass::SharedStable:
            return 0;
        }
        return 0;
    }

    void require_lane(LaneId lane, LogicalLaneState expected) const {
        if (lane.value >= lane_count_ || lanes_[lane.value] != expected ||
            ((expected == LogicalLaneState::Active ||
              expected == LogicalLaneState::TerminalPending) &&
             !active_[lane.value].occupied)) {
            throw std::logic_error("logical lane is not in the required state");
        }
    }

    [[nodiscard]] bool valid_checkpoint_summary(const auto& checkpoint,
                                                CheckpointScope scope) const noexcept {
        return checkpoint.ref.frontier != 0 && checkpoint.ref.ordinal == 0 &&
               checkpoint.scope == scope &&
               checkpoint.shortlist_key.frontier == checkpoint.ref.frontier &&
               checkpoint.required_kv.main_pages != 0 && checkpoint.rebuild_work.tokens != 0;
    }

    [[nodiscard]] bool
    valid_continuation_summary(const ContinuationSummary& summary) const noexcept {
        if ((!summary.endpoint && !summary.rewrite && summary.long_anchors.empty()) ||
            summary.long_anchors.size() > max_long_anchors_) {
            return false;
        }
        if (summary.endpoint &&
            (summary.endpoint->ref.kind != CheckpointKind::SessionEndpoint ||
             !valid_checkpoint_summary(*summary.endpoint, CheckpointScope::Private))) {
            return false;
        }
        if (summary.rewrite &&
            (summary.rewrite->ref.kind == CheckpointKind::SessionEndpoint ||
             summary.rewrite->ref.kind == CheckpointKind::SharedStablePrefix ||
             summary.rewrite->ref.kind == CheckpointKind::LongAnchor ||
             !valid_checkpoint_summary(*summary.rewrite, CheckpointScope::Private))) {
            return false;
        }
        for (std::size_t index = 0; index < summary.long_anchors.size(); ++index) {
            const auto& anchor = summary.long_anchors[index];
            if (anchor.ref.kind != CheckpointKind::LongAnchor || anchor.ref.ordinal == 0 ||
                anchor.ref.ordinal > max_long_anchors_ || anchor.ref.frontier == 0 ||
                anchor.scope != CheckpointScope::Private ||
                anchor.shortlist_key.frontier != anchor.ref.frontier ||
                anchor.required_kv.main_pages == 0 || anchor.rebuild_work.tokens == 0) {
                return false;
            }
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (summary.long_anchors[previous].ref.ordinal == anchor.ref.ordinal) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] static bool
    valid_shared_prefix_summary(const SharedPrefixSummary& summary) noexcept {
        const auto& checkpoint = summary.checkpoint;
        return checkpoint.ref.kind == CheckpointKind::SharedStablePrefix &&
               checkpoint.ref.frontier != 0 && checkpoint.ref.ordinal == 0 &&
               checkpoint.scope == CheckpointScope::Shared &&
               checkpoint.shortlist_key.frontier == checkpoint.ref.frontier &&
               checkpoint.required_kv.main_pages != 0 && checkpoint.rebuild_work.tokens != 0;
    }

    static void assign_continuation_summary(ContinuationSummary& destination,
                                            const ContinuationSummary& source) noexcept {
        if (source.long_anchors.size() > destination.long_anchors.capacity()) { std::terminate(); }
        destination.endpoint          = source.endpoint;
        destination.rewrite           = source.rewrite;
        destination.active_references = 0;
        destination.long_anchors.clear();
        for (const auto& anchor : source.long_anchors) {
            destination.long_anchors.push_back(anchor);
        }
    }

    static RetentionObservation* find_observation(std::vector<CheckpointObservation>& observations,
                                                  CheckpointRef checkpoint) noexcept {
        const auto found = std::find_if(
            observations.begin(), observations.end(),
            [&](const CheckpointObservation& value) { return value.checkpoint == checkpoint; });
        return found == observations.end() ? nullptr : &found->observation;
    }

    static const RetentionObservation*
    find_observation(const std::vector<CheckpointObservation>& observations,
                     CheckpointRef checkpoint) noexcept {
        const auto found = std::find_if(
            observations.begin(), observations.end(),
            [&](const CheckpointObservation& value) { return value.checkpoint == checkpoint; });
        return found == observations.end() ? nullptr : &found->observation;
    }

    void migrate_observations(CatalogEntry& entry, const ContinuationSummary& summary,
                              RetentionClass retention) noexcept {
        observation_scratch_.clear();
        const auto append = [&](const auto& checkpoint) {
            if (observation_scratch_.size() == observation_scratch_.capacity()) {
                std::terminate();
            }
            RetentionObservation observation{.retention_class = retention};
            if (const RetentionObservation* old =
                    find_observation(entry.observations, checkpoint.ref)) {
                observation                 = *old;
                observation.retention_class = retention;
            }
            observation_scratch_.push_back(
                CheckpointObservation{.checkpoint = checkpoint.ref, .observation = observation});
        };
        if (summary.endpoint) { append(*summary.endpoint); }
        if (summary.rewrite) { append(*summary.rewrite); }
        for (const auto& anchor : summary.long_anchors) { append(anchor); }
        entry.observations.clear();
        for (const CheckpointObservation& observation : observation_scratch_) {
            entry.observations.push_back(observation);
        }
    }

    // WHY A CATALOG CELL WAS EMPTIED -- five paths, counted separately, because inferring it from outside took
    // two rounds and settled nothing: a join proved that of 231 forks that vanished between one request and the
    // next of the same conversation, ZERO were named by an eviction line. So the removal is one of the paths
    // below, and which one decides whether it is a defect (a policy destroying state) or ordinary spend (a turn
    // consuming the fork it used and publishing a new one). `mark_terminal_pending` is expected and correct for
    // a consumed source; `apply_private_action` is the pressure path; `release_cancelled_lane` is a
    // cancellation; the other two are failure paths.
    enum class CellClearReason : std::uint8_t {
        Terminal,   // mark_terminal_pending -- a lane finished (the ordinary consume/replace)
        Action,     // apply_private_action -- the pressure path (evict/discard)
        Cancelled,  // release_cancelled_lane
        Cleanup,    // clear_after_program_cleanup
        Rollback,   // rollback_logical_materialization
        Count,
    };
    std::array<std::uint64_t, static_cast<std::size_t>(CellClearReason::Count)> cell_clears_{};

    void note_cell_clear(CellClearReason reason) noexcept {
        ++cell_clears_[static_cast<std::size_t>(reason)];
    }

    // WHY A SESSION ENTRY WAS ERASED -- a separate axis from the cell clears above, and it had NO counter at
    // all until 2026-09-28. `erase_session_if_owner` is what makes `session_cell_frontier` read 0, i.e. what
    // turns a conversation's next turn into a 23k-token re-prefill, and it is called from two unrelated places:
    // an EVICTION of the owner, and a request CONSUMING the endpoint as its active source. It is a SEPARATE
    // axis because a consume erases the session entry WITHOUT clearing the cell at all -- not because an
    // eviction's two clears take different routes, which is what this comment wrongly said first (they are
    // adjacent lines; the cell clear there simply was not counted, see the Evicted branch).
    enum class SessionEraseReason : std::uint8_t {
        Eviction,  // apply_private_action, VictimDisposition::Evicted
        Consume,   // the source was taken as an active source (ConsumeToActive)
        Count,
    };
    std::array<std::uint64_t, static_cast<std::size_t>(SessionEraseReason::Count)> session_erases_{};

    void clear_catalog_entry(CatalogEntry& entry) noexcept {
        entry.state = CatalogState::Vacant;
        entry.id    = 0;
        entry.summary.endpoint.reset();
        entry.summary.rewrite.reset();
        entry.summary.long_anchors.clear();
        entry.summary.active_references = 0;
        entry.handle.reset();
        entry.session.reset();
        entry.observations.clear();
        entry.retention = RetentionClass::RecentPrivate;
        advance_revision(entry.revision);
    }

    // Cells not `Vacant` -- catalogued, claimed, or reserved for the in-flight capture. This is the
    // occupancy half of the catalog's capacity pair, and the number the `/stats` read reports. COUNTED, not
    // tracked: a separate counter would be one more thing that can drift from `catalog_`. Called only from
    // `populate_runtime_stats`, i.e. on the engine worker under `execution_mutex_` -- the same lock planning
    // runs under -- so this walk of `catalog_` is not a concurrent read of a mutating container.
    // The owners a publication cell could be taken FROM, read from `catalog_` rather than from the planner's
    // `private_owner_ids`. THAT DISTINCTION IS THE WHOLE POINT: `private_owner_ids` is filled inside
    // `build_pressure_inputs`, which runs only if the planner reaches its pressure phase, so a run that
    // returned early (the identity path) reports ZERO owners even when the catalog is full of them -- and
    // zero owners is the reading that was designated as "the catalog really was empty". A confounded meter
    // whose decisive value can be produced by lazy construction is worse than none. This mirrors the filter
    // at `build_pressure_inputs` exactly: Catalogued, has a handle, no active edge.
    [[nodiscard]] std::uint32_t evictable_private_owners() const noexcept {
        std::uint32_t owners = 0;
        for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
            const CatalogEntry& entry = catalog_[slot];
            if (entry.state != CatalogState::Catalogued || !entry.handle ||
                private_has_active_edge(slot)) {
                continue;
            }
            ++owners;
        }
        return owners;
    }

    [[nodiscard]] std::uint32_t catalog_occupied_cells() const noexcept {
        std::uint32_t occupied = 0;
        for (const CatalogEntry& entry : catalog_) {
            if (entry.state != CatalogState::Vacant) { ++occupied; }
        }
        return occupied;
    }

    void clear_shared_entry(SharedCatalogEntry& entry) noexcept {
        entry.state = SharedCatalogState::Vacant;
        entry.id    = 0;
        entry.handle.reset();
        entry.summary     = {};
        entry.observation = RetentionObservation{.retention_class = RetentionClass::SharedStable};
        entry.transaction_pins    = 0;
        entry.explicit_credit     = false;
        entry.credit_expiry_epoch = 0;
        advance_revision(entry.revision);
    }

    void rebuild_prefix_index() {
        for (PrefixIndexEntry& entry : prefix_index_) { entry = {}; }
        std::size_t cursor = 0;
        const auto append  = [&](bool shared, std::uint32_t slot, std::uint64_t owner_id,
                                std::uint64_t revision, const auto& checkpoint) {
            if (cursor >= prefix_index_.size()) {
                throw std::logic_error("prefix index exceeded fixed capacity");
            }
            prefix_index_[cursor++] = PrefixIndexEntry{
                 .occupied   = true,
                 .shared     = shared,
                 .key        = checkpoint.shortlist_key,
                 .slot       = slot,
                 .owner_id   = owner_id,
                 .revision   = revision,
                 .checkpoint = checkpoint.ref,
            };
        };
        for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
            const CatalogEntry& entry = catalog_[slot];
            if (entry.state != CatalogState::Catalogued || !entry.handle) { continue; }
            if (entry.summary.endpoint) {
                append(false, slot, entry.id, entry.revision, *entry.summary.endpoint);
            }
            if (entry.summary.rewrite) {
                append(false, slot, entry.id, entry.revision, *entry.summary.rewrite);
            }
            for (const auto& anchor : entry.summary.long_anchors) {
                append(false, slot, entry.id, entry.revision, anchor);
            }
        }
        for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
            const SharedCatalogEntry& entry = shared_catalog_[slot];
            if (entry.state == SharedCatalogState::Catalogued && entry.handle) {
                append(true, slot, entry.id, entry.revision, entry.summary.checkpoint);
            }
        }
    }

    [[nodiscard]] bool valid_prefix_index_entry(const PrefixIndexEntry& index) const noexcept {
        if (!index.occupied) { return false; }
        if (!index.shared) {
            if (index.slot >= catalog_count_) { return false; }
            const CatalogEntry& entry = catalog_[index.slot];
            return entry.state == CatalogState::Catalogued && entry.handle &&
                   entry.id == index.owner_id && entry.revision == index.revision;
        }
        if (index.slot >= shared_catalog_count_) { return false; }
        const SharedCatalogEntry& entry = shared_catalog_[index.slot];
        return entry.state == SharedCatalogState::Catalogued && entry.handle &&
               entry.id == index.owner_id && entry.revision == index.revision;
    }

    [[nodiscard]] std::uint64_t newest_hit_epoch(const CatalogEntry& entry) const noexcept {
        std::uint64_t epoch = 0;
        for (const CheckpointObservation& observation : entry.observations) {
            epoch = std::max(epoch, observation.observation.last_hit_epoch);
        }
        return epoch;
    }

    template <class SplitCostFn>
    [[nodiscard]] std::vector<std::uint32_t> select_materialization_shared_captures(
        Program& program, const RequestBasePlan& base, const Candidate& selected_candidate,
        const RequestPlanSummary& selected_summary, const PrefixDemandRecord& provisional_demand,
        SplitCostFn&& split_cost) const {
        struct ProjectedSharedCandidate {
            PrefixShortlistKey key;
            SharedCandidateEvidence evidence = SharedCandidateEvidence::None;
            std::uint32_t frontier           = 0;
            std::uint32_t demand_mask        = 0;
            std::uint64_t rebuild_ns         = 0;
            bool pressure_capable            = false;
        };

        std::vector<ProjectedSharedCandidate> shared_candidates;
        shared_candidates.reserve(base.context_cache().opportunities.size());
        const std::uint32_t vacant_shared_slots = static_cast<std::uint32_t>(
            std::count_if(shared_catalog_.begin(), shared_catalog_.end(), [](const auto& entry) {
                return entry.state == SharedCatalogState::Vacant;
            }));
        for (const auto& opportunity : base.context_cache().opportunities) {
            if (opportunity.kind != PromptCacheMarkerKind::SharedStablePrefix ||
                opportunity.frontier < selected_summary.reusable_prompt_tokens) {
                continue;
            }
            const std::optional<PrefixShortlistKey> key =
                base.prefix_shortlist_key(opportunity.frontier);
            if (!key) { continue; }
            const bool selected_private_base =
                opportunity.frontier == selected_summary.reusable_prompt_tokens &&
                selected_candidate.private_source && selected_candidate.source_key &&
                *selected_candidate.source_key == *key;
            const bool exact_shared_resident = std::any_of(
                prefix_index_.begin(), prefix_index_.end(), [&](const PrefixIndexEntry& entry) {
                    return entry.shared && valid_prefix_index_entry(entry) && entry.key == *key;
                });
            const bool exact_resident =
                std::find(provisional_demand.exact_resident_keys.begin(),
                          provisional_demand.exact_resident_keys.end(),
                          *key) != provisional_demand.exact_resident_keys.end();
            if (exact_shared_resident || (exact_resident && !selected_private_base)) { continue; }
            const bool declared =
                has_shared_candidate_evidence(opportunity.evidence,
                                              SharedCandidateEvidence::ExplicitBoundary) ||
                has_shared_candidate_evidence(opportunity.evidence,
                                              SharedCandidateEvidence::RequestedAutomatic);
            const bool repeated = matching_reuse_domains(*key, provisional_demand) >= 2U;
            const bool surplus_candidate =
                vacant_shared_slots != 0 &&
                (has_shared_candidate_evidence(opportunity.evidence,
                                               SharedCandidateEvidence::DefaultAutomatic) ||
                 has_shared_candidate_evidence(opportunity.evidence,
                                               SharedCandidateEvidence::EngineStructural));
            if (!declared && !repeated && !surplus_candidate) { continue; }
            const std::optional<PrefillWork> rebuild =
                base.shared_candidate_rebuild_work(opportunity.frontier);
            if (!rebuild) {
                throw std::logic_error("prepared shared candidate has no canonical rebuild work");
            }
            shared_candidates.push_back(ProjectedSharedCandidate{
                .key              = *key,
                .evidence         = opportunity.evidence,
                .frontier         = opportunity.frontier,
                .demand_mask      = demand_mask_for(*key, provisional_demand),
                .rebuild_ns       = cost_model_.prefill_ns(*rebuild),
                .pressure_capable = declared || repeated,
            });
        }

        std::vector<ContextPortfolioOwnerPolicy> projected_owners;
        std::vector<ContextPortfolioCheckpointValue> projected_checkpoints;
        std::uint32_t next_projected_owner = 0;
        projected_owners.reserve(catalog_count_ + shared_catalog_count_ + shared_candidates.size());
        projected_checkpoints.reserve(prefix_index_.size() + shared_candidates.size());
        const auto append_existing = [&](PlanningOwnerId owner, const auto& handle,
                                         const auto& checkpoint) {
            const std::uint64_t rebuild  = cost_model_.prefill_ns(checkpoint.rebuild_work);
            const std::uint64_t recovery = price_checkpoint_recovery_work(
                cost_model_, program.checkpoint_recovery_work(handle, checkpoint.ref));
            projected_checkpoints.push_back(ContextPortfolioCheckpointValue{
                .owner       = owner,
                .demand_mask = demand_mask_for(checkpoint.shortlist_key, provisional_demand),
                .rebuild_ns  = rebuild,
                .baseline_recovery_ns = recovery,
                .target_recovery_ns   = recovery,
            });
        };
        for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
            const CatalogEntry& entry = catalog_[slot];
            if (entry.state != CatalogState::Catalogued || !entry.handle ||
                private_has_active_edge(slot) ||
                (selected_candidate.private_source &&
                 slot == selected_candidate.private_source->slot)) {
                continue;
            }
            const PlanningOwnerId owner{.value = next_projected_owner++};
            projected_owners.push_back(ContextPortfolioOwnerPolicy{
                .owner                    = owner,
                .private_retention_weight = private_retention_weight(entry.retention),
            });
            if (entry.summary.endpoint) {
                append_existing(owner, *entry.handle, *entry.summary.endpoint);
            }
            if (entry.summary.rewrite) {
                append_existing(owner, *entry.handle, *entry.summary.rewrite);
            }
            for (const auto& checkpoint : entry.summary.long_anchors) {
                append_existing(owner, *entry.handle, checkpoint);
            }
        }
        for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
            const SharedCatalogEntry& entry = shared_catalog_[slot];
            if (entry.state != SharedCatalogState::Catalogued || !entry.handle) { continue; }
            const PlanningOwnerId owner{.value = next_projected_owner++};
            projected_owners.push_back(ContextPortfolioOwnerPolicy{
                .owner                  = owner,
                .explicit_shared_credit = entry.explicit_credit,
            });
            append_existing(owner, *entry.handle, entry.summary.checkpoint);
        }

        std::vector<std::uint32_t> selected_frontiers;
        std::uint64_t selected_gain = 0;
        ContextPortfolioValue projected_value;
        if (shared_candidates.size() > 7U) {
            throw std::logic_error("prepared shared candidates exceeded the fixed subset bound");
        }
        const std::uint32_t subset_count = 1U << shared_candidates.size();
        for (std::uint32_t mask = 1; mask < subset_count; ++mask) {
            const std::uint32_t selected_count = std::popcount(mask);
            if (selected_count > shared_catalog_count_) { continue; }
            std::uint32_t surplus_only_count = 0;
            std::vector<std::uint32_t> frontiers;
            frontiers.reserve(selected_count);
            std::vector<ContextPortfolioOwnerPolicy> owners          = projected_owners;
            std::vector<ContextPortfolioCheckpointValue> checkpoints = projected_checkpoints;
            for (std::size_t index = 0; index < shared_candidates.size(); ++index) {
                if ((mask & (1U << index)) == 0) { continue; }
                const ProjectedSharedCandidate& candidate = shared_candidates[index];
                if (!candidate.pressure_capable) { ++surplus_only_count; }
                frontiers.push_back(candidate.frontier);
                const PlanningOwnerId owner{.value = next_projected_owner +
                                                     static_cast<std::uint32_t>(index)};
                const bool credit =
                    has_shared_candidate_evidence(candidate.evidence,
                                                  SharedCandidateEvidence::ExplicitBoundary) ||
                    has_shared_candidate_evidence(candidate.evidence,
                                                  SharedCandidateEvidence::RequestedAutomatic);
                owners.push_back(ContextPortfolioOwnerPolicy{
                    .owner                  = owner,
                    .explicit_shared_credit = credit,
                });
                checkpoints.push_back(ContextPortfolioCheckpointValue{
                    .owner                = owner,
                    .demand_mask          = candidate.demand_mask,
                    .rebuild_ns           = candidate.rebuild_ns,
                    .baseline_recovery_ns = candidate.rebuild_ns,
                    .target_recovery_ns   = 0,
                });
            }
            if (surplus_only_count > vacant_shared_slots) { continue; }
            std::sort(frontiers.begin(), frontiers.end());
            const ContextPortfolioValueResult value = projected_value.fold(owners, checkpoints);
            const std::uint64_t schedule_cost       = split_cost(frontiers);
            if (value.saturated ||
                value.private_transition_loss >
                    std::numeric_limits<std::uint64_t>::max() - value.baseline_public_value) {
                continue;
            }
            std::uint64_t threshold = value.baseline_public_value + value.private_transition_loss;
            if (schedule_cost > std::numeric_limits<std::uint64_t>::max() - threshold) { continue; }
            threshold += schedule_cost;
            if (value.target_public_value <= threshold) { continue; }
            const std::uint64_t gain = value.target_public_value - threshold;
            const bool better =
                gain > selected_gain ||
                (gain == selected_gain &&
                 (selected_frontiers.empty() || frontiers.size() < selected_frontiers.size() ||
                  (frontiers.size() == selected_frontiers.size() &&
                   std::lexicographical_compare(frontiers.begin(), frontiers.end(),
                                                selected_frontiers.begin(),
                                                selected_frontiers.end()))));
            if (better) {
                selected_gain      = gain;
                selected_frontiers = std::move(frontiers);
            }
        }
        return selected_frontiers;
    }

    [[nodiscard]] std::optional<Choice>
    plan_materialization(Program& program, const PreparedPrompt& prompt,
                         const RequestBasePlan& base, LaneId destination,
                         std::vector<Candidate>& candidates, const CandidateCounters& candidate_counters,
                         std::uint64_t publication_order,
                         typename Planner::Clock::time_point planning_started,
                         PrefixDemandRecord& provisional_demand, PlanningAllowance allowance) {
        std::vector<typename Planner::CandidateInput> candidate_inputs;
        std::vector<const ContinuationHandle*> private_owners;
        std::vector<PlanningOwnerId> private_owner_ids;
        std::vector<const SharedPrefixHandle*> shared_owners;
        std::vector<PlanningOwnerId> shared_owner_ids;
        std::vector<PlanningOwnerRecord> owner_records;
        std::vector<MaterializationOwnerPolicy> owner_policies;
        std::vector<MaterializationCheckpointPolicy> checkpoint_policies;
        candidate_inputs.reserve(candidates.size());

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (!candidates[index].plan) {
                throw std::logic_error("materialization candidate is empty");
            }
            candidate_inputs.push_back(typename Planner::CandidateInput{
                .candidate      = &*candidates[index].plan,
                .id             = PlanningCandidateId{.value = static_cast<std::uint32_t>(index)},
                .stable_ordinal = static_cast<std::uint32_t>(index),
                .current_session_binding = candidates[index].current_session_binding,
            });
        }

        bool pressure_inputs_built       = false;
        const auto build_pressure_inputs = [&]() -> typename Planner::PressureInputs {
            if (pressure_inputs_built) {
                throw std::logic_error("materialization pressure inputs requested twice");
            }
            pressure_inputs_built = true;
            private_owners.reserve(catalog_count_);
            private_owner_ids.reserve(catalog_count_);
            shared_owners.reserve(shared_catalog_count_);
            shared_owner_ids.reserve(shared_catalog_count_);
            owner_records.reserve(catalog_count_ + shared_catalog_count_);
            owner_policies.reserve(catalog_count_ + shared_catalog_count_);
            checkpoint_policies.reserve(prefix_index_.size());

            for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
                const CatalogEntry& entry = catalog_[slot];
                if (entry.state != CatalogState::Catalogued || !entry.handle ||
                    private_has_active_edge(slot)) {
                    continue;
                }
                const PlanningOwnerId owner{.value =
                                                static_cast<std::uint32_t>(owner_records.size())};
                private_owners.push_back(&*entry.handle);
                private_owner_ids.push_back(owner);
                owner_records.push_back(PlanningOwnerRecord{
                    .id = owner,
                    .capability =
                        CatalogCapability{
                            .owner =
                                LogicalOwnerKey{
                                    .kind = LogicalOwnerKind::PrivateContinuation,
                                    .id   = entry.id,
                                },
                            .slot       = slot,
                            .generation = entry.revision,
                        },
                });
                std::uint64_t selected_hits  = 0;
                const auto append_checkpoint = [&](const auto& checkpoint) {
                    const RetentionObservation* observation =
                        find_observation(entry.observations, checkpoint.ref);
                    if (observation == nullptr) {
                        throw std::logic_error("catalogued checkpoint has no policy observation");
                    }
                    selected_hits = std::max(selected_hits, observation->selected_hit_count);
                    checkpoint_policies.push_back(MaterializationCheckpointPolicy{
                        .owner              = owner,
                        .checkpoint         = checkpoint.ref,
                        .retention_class    = observation->retention_class,
                        .selected_hit_count = observation->selected_hit_count,
                        .last_hit_epoch     = observation->last_hit_epoch,
                        .demand_mask =
                            demand_mask_for(checkpoint.shortlist_key, provisional_demand),
                        .rebuild_ns           = cost_model_.prefill_ns(checkpoint.rebuild_work),
                        .baseline_recovery_ns = price_checkpoint_recovery_work(
                            cost_model_,
                            program.checkpoint_recovery_work(*entry.handle, checkpoint.ref)),
                    });
                };
                if (entry.summary.endpoint) { append_checkpoint(*entry.summary.endpoint); }
                if (entry.summary.rewrite) { append_checkpoint(*entry.summary.rewrite); }
                for (const auto& checkpoint : entry.summary.long_anchors) {
                    append_checkpoint(checkpoint);
                }
                owner_policies.push_back(MaterializationOwnerPolicy{
                    .owner                    = owner,
                    .retention_class          = entry.retention,
                    .selected_hit_count       = selected_hits,
                    .last_hit_epoch           = newest_hit_epoch(entry),
                    .private_retention_weight = private_retention_weight(entry.retention),
                });
            }
            for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                const SharedCatalogEntry& entry = shared_catalog_[slot];
                if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                    entry.transaction_pins != 0 || shared_active_edge_count(slot) != 0) {
                    continue;
                }
                const PlanningOwnerId owner{.value =
                                                static_cast<std::uint32_t>(owner_records.size())};
                shared_owners.push_back(&*entry.handle);
                shared_owner_ids.push_back(owner);
                owner_records.push_back(PlanningOwnerRecord{
                    .id = owner,
                    .capability =
                        CatalogCapability{
                            .owner =
                                LogicalOwnerKey{
                                    .kind = LogicalOwnerKind::SharedPrefix,
                                    .id   = entry.id,
                                },
                            .slot       = slot,
                            .generation = entry.revision,
                        },
                });
                owner_policies.push_back(MaterializationOwnerPolicy{
                    .owner                    = owner,
                    .retention_class          = RetentionClass::SharedStable,
                    .selected_hit_count       = entry.observation.selected_hit_count,
                    .last_hit_epoch           = entry.observation.last_hit_epoch,
                    .private_retention_weight = 0,
                    .explicit_shared_credit   = entry.explicit_credit,
                });
                checkpoint_policies.push_back(MaterializationCheckpointPolicy{
                    .owner              = owner,
                    .checkpoint         = entry.summary.checkpoint.ref,
                    .retention_class    = RetentionClass::SharedStable,
                    .selected_hit_count = entry.observation.selected_hit_count,
                    .last_hit_epoch     = entry.observation.last_hit_epoch,
                    .demand_mask =
                        demand_mask_for(entry.summary.checkpoint.shortlist_key, provisional_demand),
                    .rebuild_ns = cost_model_.prefill_ns(entry.summary.checkpoint.rebuild_work),
                    .baseline_recovery_ns = price_checkpoint_recovery_work(
                        cost_model_, program.checkpoint_recovery_work(
                                         *entry.handle, entry.summary.checkpoint.ref)),
                });
            }

            return typename Planner::PressureInputs{
                .private_owners    = private_owners,
                .private_owner_ids = private_owner_ids,
                .shared_owners     = shared_owners,
                .shared_owner_ids  = shared_owner_ids,
                .owner_policy      = owner_policies,
                .checkpoint_policy = checkpoint_policies,
            };
        };

        // The probe, split from the goal so that a FAILURE'S REASON survives: the planner calls this from five
        // sites and most calls are the search exercising an option, so what matters is not how often it
        // fails but whether the cell was the ONLY thing that could have blocked a candidate. `cell_only`
        // marks the one failure that is the catalog's doing; every other return is `false`.
        struct GoalProbe {
            std::optional<typename Planner::LogicalGoal> goal;
            std::size_t candidate_index = kNoCandidateIndex;
            bool cell_only              = false;
        };
        const auto logical_goal_probe = [&](PlanningCandidateId candidate_id,
                                            PrivateSourceMode source_mode,
                                            std::span<const PressureOwnerOutcome> outcomes) -> GoalProbe {
            const auto candidate_record =
                std::find_if(candidate_inputs.begin(), candidate_inputs.end(),
                             [&](const typename Planner::CandidateInput& input) {
                                 return input.id == candidate_id;
                             });
            if (candidate_record == candidate_inputs.end()) { return GoalProbe{}; }
            const std::size_t candidate_index =
                static_cast<std::size_t>(candidate_record - candidate_inputs.begin());
            const Candidate& candidate = candidates[candidate_index];
            if (candidate.shared_source && source_mode != PrivateSourceMode::Retain) {
                return GoalProbe{.candidate_index = candidate_index};
            }
            if (!candidate.private_source && !candidate.shared_source &&
                source_mode == PrivateSourceMode::Retain) {
                return GoalProbe{.candidate_index = candidate_index};
            }
            if (candidate.private_source && source_mode != PrivateSourceMode::Retain &&
                source_mode != PrivateSourceMode::ConsumeToActive) {
                return GoalProbe{.candidate_index = candidate_index};
            }

            std::uint32_t publication_slot = kInvalidCatalogSlot;
            if (candidate.private_source && source_mode == PrivateSourceMode::ConsumeToActive) {
                publication_slot = candidate.private_source->slot;
            } else {
                for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
                    if (catalog_[slot].state == CatalogState::Vacant) {
                        publication_slot = slot;
                        break;
                    }
                }
            }

            for (std::size_t row = 0; row < outcomes.size(); ++row) {
                const PressureOwnerOutcome& outcome = outcomes[row];
                if (outcome.disposition != VictimDisposition::Retained &&
                    outcome.disposition != VictimDisposition::Evicted) {
                    return GoalProbe{.candidate_index = candidate_index};
                }
                if (std::find_if(outcomes.begin(), outcomes.begin() + row,
                                 [&](const PressureOwnerOutcome& prior) {
                                     return prior.owner == outcome.owner;
                                 }) != outcomes.begin() + row) {
                    return GoalProbe{.candidate_index = candidate_index};
                }
                const auto record = std::find_if(
                    owner_records.begin(), owner_records.end(),
                    [&](const PlanningOwnerRecord& item) { return item.id == outcome.owner; });
                if (record == owner_records.end()) { return GoalProbe{.candidate_index = candidate_index}; }
                const bool shared = record->capability.owner.kind == LogicalOwnerKind::SharedPrefix;
                if (!shared) {
                    const std::uint32_t slot = record->capability.slot;
                    if (slot >= catalog_count_ ||
                        (candidate.private_source && slot == candidate.private_source->slot)) {
                        return GoalProbe{.candidate_index = candidate_index};
                    }
                    const CatalogEntry& entry = catalog_[slot];
                    if (entry.state != CatalogState::Catalogued || !entry.handle ||
                        entry.id != record->capability.owner.id ||
                        entry.revision != record->capability.generation ||
                        private_has_active_edge(slot)) {
                        return GoalProbe{.candidate_index = candidate_index};
                    }
                    if (publication_slot == kInvalidCatalogSlot &&
                        outcome.disposition == VictimDisposition::Evicted) {
                        publication_slot = slot;
                    }
                } else {
                    const std::uint32_t slot = record->capability.slot;
                    if (slot >= shared_catalog_count_ ||
                        (candidate.shared_source && slot == candidate.shared_source->slot)) {
                        return GoalProbe{.candidate_index = candidate_index};
                    }
                    const SharedCatalogEntry& entry = shared_catalog_[slot];
                    if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                        entry.id != record->capability.owner.id ||
                        entry.revision != record->capability.generation ||
                        entry.transaction_pins != 0 || shared_active_edge_count(slot) != 0) {
                        return GoalProbe{.candidate_index = candidate_index};
                    }
                }
            }
            if (publication_slot == kInvalidCatalogSlot) {
                // THE ONE FAILURE THAT IS THE CATALOG'S DOING -- reported as a REASON, not counted here. This
                // site is reached only after every other validation passed, so it is precisely "this
                // candidate could have been adopted if a cell had been obtainable". Whether that COST
                // anything is not decidable at this point and is not decided here: the planner is allowed to
                // probe this option on the way to taking an eviction, and it does so thousands of times per
                // request. The loss is decided once per planning run, in `publication_cell_loss` below. (The
                // first version of this instrument counted THESE calls, reached 108,544 across 24 requests,
                // and printed a "dropped" line for candidates that then won -- see the header.)
                return GoalProbe{.candidate_index = candidate_index, .cell_only = true};
            }
            return GoalProbe{.goal = typename Planner::LogicalGoal{.publication_slot = publication_slot},
                             .candidate_index = candidate_index};
        };

        // The tally the planner's calls accumulate into, and the wrapper that is what the planner sees. The
        // wrapper's signature is the one the five call sites already use, so nothing in the planner changed.
        std::vector<PublicationCellProbe> probe_tally;
        const auto logical_goal = [&](PlanningCandidateId candidate_id, PrivateSourceMode source_mode,
                                      std::span<const PressureOwnerOutcome> outcomes)
            -> std::optional<typename Planner::LogicalGoal> {
            const GoalProbe probe = logical_goal_probe(candidate_id, source_mode, outcomes);
            if (probe.candidate_index != kNoCandidateIndex) {
                if (probe.candidate_index >= probe_tally.size()) {
                    probe_tally.resize(probe.candidate_index + 1U);
                }
                PublicationCellProbe& tally = probe_tally[probe.candidate_index];
                ++tally.probes;
                if (probe.goal) {
                    ++tally.goals;
                } else if (probe.cell_only) {
                    ++tally.cell_only;
                } else {
                    ++tally.other;
                }
            }
            return probe.goal;
        };

        const auto final_schedule = [&](PlanningCandidateId candidate_id,
                                        const RequestPlanSummary& summary,
                                        const auto& split_cost) -> std::vector<std::uint32_t> {
            const auto selected = std::find_if(candidate_inputs.begin(), candidate_inputs.end(),
                                               [&](const typename Planner::CandidateInput& input) {
                                                   return input.id == candidate_id;
                                               });
            if (selected == candidate_inputs.end()) {
                throw std::logic_error("final schedule references an unknown candidate");
            }
            const Candidate& candidate =
                candidates[static_cast<std::size_t>(selected - candidate_inputs.begin())];
            return select_materialization_shared_captures(program, base, candidate, summary,
                                                          provisional_demand, split_cost);
        };

        std::optional<typename Planner::Result> planned =
            planner_.plan(program, prompt, cost_model_, candidate_inputs, 0, build_pressure_inputs,
                          logical_goal, final_schedule, planning_started, allowance);
        // The probe denominator is added BEFORE the early return, so a planning run that produced no plan still
        // contributes its probes. Conditioning it on success made "probed but never planned" look identical to
        // "stopped probing" -- the one thing a denominator exists to distinguish.
        // Declared OUTSIDE the block below because the loss prints further down quote it; a second sum for
        // them would be a second place for the number to drift.
        std::uint64_t probes_total = 0;
        {
            std::uint64_t blocked_cell  = 0;
            std::uint64_t blocked_other = 0;
            for (const PublicationCellProbe& tally : probe_tally) {
                probes_total += tally.probes;
                blocked_cell += tally.cell_only;
                blocked_other += tally.other;
            }
            program.add_publication_cell_probes(probes_total);
            // THE GOAL-FAILURE SPLIT BELONGS HERE, IN THE SAME UNCONDITIONAL BLOCK AND FOR THE SAME REASON
            // AS THE PROBES DENOMINATOR: a planning run that produced NO plan still failed its goals, and
            // those are exactly the runs whose reasons matter. The first version of this call sat after the
            // `!planned` early return below, so it fired only when a plan had been produced -- and read 0 on
            // real traffic while `assessed_targets_without_goal` was 1266/request and the probe denominator
            // was 295,251. **The instrument measured nothing and the zero looked like an answer**, which is
            // the failure mode this file's own comment above describes. Keep the two sided by side.
            program.add_publication_goal_blocked(blocked_cell, blocked_other);
        }
        const auto selected_candidate =
            planned ? std::find_if(candidate_inputs.begin(), candidate_inputs.end(),
                                   [&](const typename Planner::CandidateInput& input) {
                                       return input.id == planned->candidate;
                                   })
                    : candidate_inputs.end();
        if (!planned || !planned->plan || selected_candidate == candidate_inputs.end()) {
            return std::nullopt;
        }

        Candidate& candidate =
            candidates[static_cast<std::size_t>(selected_candidate - candidate_inputs.begin())];

        // DID THE CATALOG COST THIS REQUEST ANY REUSE? Decided ONCE, here, after the winner is known -- and
        // the whole of the instrument is this one question, because the raw probe count answers a different
        // one. `probe_tally` holds, per candidate, how many times the goal builder failed for it and WHY:
        // a candidate whose every failure was `cell_only` could not be adopted at all, so if it would have
        // reused more than the plan that won, the cell took that reuse away. The winner is excluded: its own
        // probes failing on the cell is the ordinary eviction-to-publish path, which is expected.
        //
        // The comparison is on `reusable_prompt_tokens`, the same quantity the selection instrument uses.
        {
            const std::size_t winner_index = static_cast<std::size_t>(&candidate - candidates.data());
            const std::uint32_t winner_reuse =
                candidate.plan ? candidate.plan->summary().reusable_prompt_tokens : 0U;
            const auto reuse_of = [&](std::size_t index) -> std::uint32_t {
                return candidates[index].plan ? candidates[index].plan->summary().reusable_prompt_tokens
                                              : 0U;
            };
            const PublicationCellLoss loss =
                publication_cell_loss(probe_tally, candidates.size(), winner_index, winner_reuse, reuse_of);
            // (The unconditional goal-failure split is tallied above, beside the probes denominator -- see
            // the block at the early return for why it must not live down here.)
            // THE AT-RISK LINE IS LOG-ONLY AND DELIBERATELY NOT AN ALERT (see the awk). Its purpose is to
            // settle whether a catalog-caused loss is REACHABLE at this configuration: `evictable_owners` is
            // the count of private owner records the planner had -- i.e. owners it could have taken a cell
            // from -- so `at_risk` positive WITH `evictable_owners=0` is the mechanism real, and `at_risk`
            // positive only ever with owners available says a cell was obtainable and this predicate is
            // measuring search truncation instead. Until that is measured, arming on it would put an
            // uninterpreted number in the alert stream.
            // TALLIED UNCONDITIONALLY, and this is not tidiness: the PRINT is capped at 8 then every 512th, so
            // the printed line count is a SAMPLE and never the count. Reading "8 at-risk runs" off eight lines
            // is the exact error CLAUDE.md records for the eviction print, and it was made here before this
            // counter existed. `/stats` carries the totals; the journal carries an example.
            if (loss.at_risk != 0U) {
                program.add_publication_cell_at_risk(loss.at_risk, loss.blocked_by_goals, loss.blocked_by_other,
                                                     loss.blocked_by_reuse);
                const std::uint64_t at_risk_seen = program.note_publication_cell_at_risk();
                if (at_risk_seen <= 8U || at_risk_seen % 512U == 0U) {
                    std::fprintf(stderr,
                                 "[engine] catalog cell at-risk: occupied=%u/%u at_risk=%u evictable_owners=%u "
                                 "owners_enumerated=%zu veto_goals=%u veto_other=%u veto_reuse=%u counted=%u "
                                 "probes=%llu seen=%llu\n",
                                 catalog_occupied_cells(), catalog_count_, loss.at_risk,
                                 evictable_private_owners(), private_owner_ids.size(),
                                 loss.blocked_by_goals, loss.blocked_by_other, loss.blocked_by_reuse,
                                 loss.candidates, static_cast<unsigned long long>(probes_total),
                                 static_cast<unsigned long long>(at_risk_seen));
                    std::fflush(stderr);
                }
            }
            if (loss.candidates != 0U) {
                const std::uint64_t losses = program.note_publication_cell_loss();
                // Rate-limited to the first 8 then every 512th, like the eviction print; the `/stats` counter
                // is the un-muted reading, so silence after the 8th means "not printed", never "not recurring".
                if (losses <= 8U || losses % 512U == 0U) {
                    std::fprintf(stderr,
                                 "[engine] catalog cell blocked reuse: occupied=%u/%u candidates=%u "
                                 "best_blocked_reuse=%u chosen_reuse=%u probes=%llu losses=%llu\n",
                                 catalog_occupied_cells(), catalog_count_, loss.candidates,
                                 loss.best_blocked_reuse, winner_reuse,
                                 static_cast<unsigned long long>(probes_total),
                                 static_cast<unsigned long long>(losses));
                    std::fflush(stderr);
                }
            }
        }

        // THE DECIDING COMPARISON (2026-09-27). The reuse-choice instrument (`request_plan.cpp`) established
        // that a LONGER private continuation is alive when the shared snapshot is taken, and that is all it
        // could say -- which candidate won, and by what, was invisible. This reports the winner beside the
        // best LOSING candidate, by the one quantity that says how much reuse each would have saved.
        //
        // It exists because a fix was attempted on a guess: `FoldedCost::key()` was reordered to promote
        // `remaining_text_prefill` above the hit/credit terms, and after a rebuild the selections were
        // BYTE-IDENTICAL -- so that key does not decide this comparison, or the term does not differ between
        // these candidates. Reading the decision beats guessing at it, and this is the read.
        //
        // `longer_lost=1` is the defect: a candidate reusing strictly more tokens was available and lost.
        // Populated for the REQUEST LOG, not just the rate-limited line below. See the field's comment in
        // `types.h`: the journal line answers "was a longer candidate refused" only for the first 8
        // selections per process, which is precisely when nobody is asking.
        std::vector<MaterializationDiagnostics::MaterializationCandidate> candidate_rows;
        std::uint32_t selection_chosen_reuse = 0;
        std::uint32_t selection_best_loser   = 0;
        bool          selection_longer_lost  = false;
        {
            static std::uint64_t selections = 0;
            ++selections;
            // COMPUTE FIRST, THEN DECIDE WHETHER TO SPEAK -- the first version tested the condition before the
            // loop that produces it, so the interesting case could not have been selected for anyway.
            const auto reuse_of = [](const Candidate& item) -> std::uint32_t {
                return item.plan ? item.plan->summary().reusable_prompt_tokens : 0U;
            };
            const std::uint32_t winner_reuse       = reuse_of(candidate);
            std::uint32_t       best_other_reuse   = 0U;
            bool                best_other_is_shared = false;
            for (const Candidate& other : candidates) {
                if (&other == &candidate) { continue; }
                const std::uint32_t other_reuse = reuse_of(other);
                if (other_reuse > best_other_reuse) {
                    best_other_reuse     = other_reuse;
                    best_other_is_shared = other.shared_source.has_value();
                }
            }
            const bool longer_lost = best_other_reuse > winner_reuse;
            selection_chosen_reuse = winner_reuse;
            selection_best_loser   = best_other_reuse;
            selection_longer_lost  = longer_lost;
            candidate_rows.reserve(candidates.size());
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                const Candidate& item = candidates[index];
                MaterializationDiagnostics::MaterializationCandidate row;
                row.reuse          = item.plan ? item.plan->summary().reusable_prompt_tokens : 0U;
                row.winner         = &item == &candidate;
                row.private_source = item.private_source.has_value();
                row.shared_source  = item.shared_source.has_value();
                if (index < probe_tally.size()) {
                    row.probes    = probe_tally[index].probes;
                    row.goals     = probe_tally[index].goals;
                    row.cell_only = probe_tally[index].cell_only;
                    row.other     = probe_tally[index].other;
                }
                candidate_rows.push_back(row);
            }
            // `longer_lost` is the ONLY case worth acting on, so it is never rate-limited away. The 8-sample
            // limit meant "the planner consistently picks the longer candidate" rested on the process's first
            // eight admissions -- before any shared prefix was even captured -- while ~70 later selections
            // went unprinted. A conclusion drawn from the unrepresentative 12% is how that claim got made.
            if (selections <= 8U || selections % 512U == 0U || longer_lost) {
                std::fprintf(stderr,
                             "[engine] reuse-select: winner=%s reuse=%u | best_loser=%s reuse=%u "
                             "longer_lost=%d candidates=%zu selects=%llu\n",
                             candidate.shared_source.has_value() ? "shared" : "private", winner_reuse,
                             best_other_is_shared ? "shared" : "private", best_other_reuse,
                             static_cast<int>(longer_lost), candidates.size(),
                             static_cast<unsigned long long>(selections));
                std::fflush(stderr);
            }
        }

        Choice choice(destination, std::move(*planned->plan), catalog_count_,
                      base.context_cache().session_key, base.context_cache().retention,
                      base.context_cache().update_session_index, publication_order);
        choice.private_source_                 = candidate.private_source;
        choice.source_mode_                    = planned->source_mode;
        choice.shared_source_                  = candidate.shared_source;
        choice.publication_slot_               = planned->publication_slot;
        choice.selected_observation_           = candidate.selected_observation;
        choice.diagnostics_                    = planned->diagnostics;
        // THE SPLIT, over the catalog itself. Reported per request because §2f's verdict is per request:
        // whether the deepest match is shallow (the prompt diverged) or deep with a shallow restorable
        // frontier (ours). Every stored entry is scanned, not only the ones the search happened to offer.
        {
            // THE SESSION CELL vs THE CANDIDATES, computed here where both are in scope: the cell holds the
            // conversation's NEWEST published continuation, and this says what it holds and whether it was
            // offered. `candidates` carries each one's slot through `private_source`.
            std::uint32_t session_cell_frontier = 0;
            bool          session_cell_offered  = false;
            if (cache_enabled_ && base.context_cache().session_key &&
                base.context_cache().update_session_index) {
                const std::optional<std::size_t> cell = find_session_cell(*base.context_cache().session_key);
                if (cell && session_index_[*cell].slot < catalog_count_) {
                    const std::uint32_t slot  = session_index_[*cell].slot;
                    const CatalogEntry& entry = catalog_[slot];
                    if (entry.state == CatalogState::Catalogued && entry.summary.endpoint) {
                        session_cell_frontier = entry.summary.endpoint->ref.frontier;
                        for (const Candidate& offered : candidates) {
                            if (offered.private_source && offered.private_source->slot == slot) {
                                session_cell_offered = true;
                                break;
                            }
                        }
                    }
                }
            }
            std::vector<PrefixSplitSample> samples;
            samples.reserve(static_cast<std::size_t>(catalog_count_) + shared_catalog_count_);
            // `source` says WHOSE ledger this sample is. It is taken from the SESSION INDEX -- the cell this
            // request's key resolves to -- and NOT from `entry.session`, which is the wrong field and was the
            // first version of this: `demote_replaced_session` does `prior.session.reset()` every time a newer
            // continuation publishes, so this conversation's own SUPERSEDED ledgers carry no session marker and
            // were being labelled "another session's". That inflates the very population this tag exists to
            // measure. 1 = the cell this request's key resolves to, 2 = any other private entry (which
            // includes this conversation's superseded ones -- they are no longer its cell), 3 = shared.
            const std::uint32_t own_cell_slot = [&]() -> std::uint32_t {
                if (!cache_enabled_ || !base.context_cache().session_key) {
                    return kInvalidCatalogSlot;
                }
                const std::optional<std::size_t> cell =
                    find_session_cell(*base.context_cache().session_key);
                return cell && session_index_[*cell].slot < catalog_count_
                           ? session_index_[*cell].slot
                           : kInvalidCatalogSlot;
            }();
            const auto consider = [&](const Program::PrefixSplit& split, std::uint8_t source) {
                samples.push_back(PrefixSplitSample{.tokens      = split.tokens,
                                                    .restorable  = split.restorable,
                                                    .identity_ok = split.identity_ok,
                                                    .match_end   = split.match_end,
                                                    .stored      = split.stored,
                                                    .source      = source,
                                                    // THIS entry's own restorable point at or below its match --
                                                    // NOT the resume point and NOT comparable to
                                                    // `session_cell_frontier`. `prefix_split` skips any
                                                    // checkpoint whose frontier exceeds the match, so this is
                                                    // <= match <= the prompt by construction: "a frontier beyond
                                                    // the prompt" is impossible here, although the first
                                                    // comment on this field claimed it could be read that way.
                                                    .frontier    = split.restorable,
                                                    .probe_index = split.probe_index,
                                                    // THIS entry's own divergence, carried here for
                                                    // the reason `source` and `frontier` are.
                                                    .divergence  = split.divergence,
});
            };
            for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
                const CatalogEntry& entry = catalog_[slot];
                if (entry.state != CatalogState::Catalogued || !entry.handle) { continue; }
                consider(program.prefix_split(*entry.handle, prompt), slot == own_cell_slot ? 1U : 2U);
            }
            for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                const SharedCatalogEntry& entry = shared_catalog_[slot];
                if (entry.state != SharedCatalogState::Catalogued || !entry.handle) { continue; }
                consider(program.prefix_split(*entry.handle, prompt), 3U);
            }
            const PrefixSplitBest best = best_prefix_split(samples);
            choice.diagnostics_.split_best_tokens     = best.tokens;
            choice.diagnostics_.split_best_restorable = best.restorable;
            choice.diagnostics_.split_entries         = best.entries;
            choice.diagnostics_.split_identity_ok     = best.identity_ok;
            // WHY the deepest match stopped: a field whose zero and whose finding look identical gets read as
            // whichever the reader expects -- and it was, by me, as "61% of prompts diverge inside the region
            // they should share", when nearly every case was a ledger ending or a missing entry.
            choice.diagnostics_.split_ended_by         = best.match_end;
            choice.diagnostics_.split_best_stored      = best.stored;
            choice.diagnostics_.split_best_source      = best.source;
            choice.diagnostics_.split_best_frontier    = best.frontier;
            choice.diagnostics_.split_probe_index      = best.probe_index;
            choice.diagnostics_.split_message_index    = best.divergence.message_index;
            choice.diagnostics_.split_message_offset   = best.divergence.message_offset;
            choice.diagnostics_.split_message_role     = best.divergence.role;
            // COVERAGE, STATED SO IT IS NOT ASSUMED. This assignment and the two above it are pinned by
            // `test_prefix_split_diagnostics_follow_catalog` -- EXCEPT this one: that probe's divergence
            // lands INSIDE a message, so `past_last_message` is the default and a mutant deleting this
            // line survives the manager suite. A probe at the last frontier was attempted and abandoned:
            // every frontier array that puts the divergence at the end (a two-entry {0,5}, or a
            // shorter last entry) made that fixture report NO CHOICE at all, so the case could not be
            // reached there. What IS pinned is the value's journey onward -- `test_request_log.cpp`
            // asserts the key with a non-default `true` -- and the mapping itself is pinned exhaustively
            // in `test_materialization_budget.cpp`. The gap is exactly one line, here, and it is known.
            choice.diagnostics_.split_past_last_message = best.divergence.past_last_message;
            choice.diagnostics_.session_cell_frontier = session_cell_frontier;
            choice.diagnostics_.session_cell_offered  = session_cell_offered;
            choice.diagnostics_.session_cell_skip     = candidate_counters.session_cell_skip;
            choice.diagnostics_.session_endpoint_skip = candidate_counters.session_endpoint_skip;
            choice.diagnostics_.sibling_candidates    = candidate_counters.sibling_candidates;
            choice.diagnostics_.retained_sources      = candidate_counters.retained_sources;
            choice.diagnostics_.consumed_sources      = candidate_counters.consumed_sources;
        }
        choice.diagnostics_.candidates         = std::move(candidate_rows);
        choice.diagnostics_.chosen_reuse       = selection_chosen_reuse;
        choice.diagnostics_.best_loser_reuse   = selection_best_loser;
        choice.diagnostics_.longer_lost        = selection_longer_lost;
        provisional_demand.selected_source_key = candidate.source_key;
        choice.demand_                         = std::move(provisional_demand);
        for (const PressureOwnerOutcome& outcome : planned->owner_outcomes) {
            const auto record = std::find_if(
                owner_records.begin(), owner_records.end(),
                [&](const PlanningOwnerRecord& item) { return item.id == outcome.owner; });
            if (record == owner_records.end()) {
                throw std::logic_error("selected pressure outcome has no logical owner record");
            }
            const bool shared = record->capability.owner.kind == LogicalOwnerKind::SharedPrefix;
            if (!shared) {
                const CatalogEntry& entry          = catalog_.at(record->capability.slot);
                std::vector<CheckpointRef> dropped = selected_checkpoint_drops(
                    outcome.owner, outcome.disposition, outcome.dropped_checkpoints,
                    planned->checkpoint_outcomes, continuation_checkpoint_count(entry.summary),
                    [&](CheckpointRef checkpoint) {
                        return continuation_contains_checkpoint(entry.summary, checkpoint);
                    });
                choice.private_claims_.push_back(OwnerClaim{
                    .planning_id         = outcome.owner,
                    .capability          = record->capability,
                    .disposition         = outcome.disposition,
                    .dropped_checkpoints = std::move(dropped),
                });
            } else {
                const SharedCatalogEntry& entry    = shared_catalog_.at(record->capability.slot);
                std::vector<CheckpointRef> dropped = selected_checkpoint_drops(
                    outcome.owner, outcome.disposition, outcome.dropped_checkpoints,
                    planned->checkpoint_outcomes, 1U, [&](CheckpointRef checkpoint) {
                        return checkpoint == entry.summary.checkpoint.ref;
                    });
                choice.shared_claims_.push_back(OwnerClaim{
                    .planning_id         = outcome.owner,
                    .capability          = record->capability,
                    .disposition         = outcome.disposition,
                    .dropped_checkpoints = std::move(dropped),
                });
            }
        }
        return choice;
    }

    void validate_choice(const Choice& choice, ProgramResourceRevision revision) const {
        if (!choice.plan_ || choice.destination_.value >= lane_count_ ||
            lanes_[choice.destination_.value] != LogicalLaneState::Free || revision.value == 0 ||
            choice.publication_slot_ >= catalog_count_ || choice.publication_order_ == 0) {
            throw std::logic_error("resource choice is stale or malformed");
        }
        if (choice.private_source_) {
            const CatalogCapability& capability = *choice.private_source_;
            if (capability.owner.kind != LogicalOwnerKind::PrivateContinuation ||
                capability.slot >= catalog_count_) {
                throw std::logic_error("private source slot is invalid");
            }
            const CatalogEntry& source = catalog_[capability.slot];
            if (source.state != CatalogState::Catalogued || !source.handle ||
                source.id != capability.owner.id || source.revision != capability.generation ||
                private_has_active_edge(capability.slot)) {
                throw std::logic_error("private source changed after planning");
            }
        }
        if (choice.shared_source_) {
            const CatalogCapability& capability = *choice.shared_source_;
            if (capability.owner.kind != LogicalOwnerKind::SharedPrefix ||
                capability.slot >= shared_catalog_count_) {
                throw std::logic_error("shared source slot is invalid");
            }
            const SharedCatalogEntry& source = shared_catalog_[capability.slot];
            if (source.state != SharedCatalogState::Catalogued || !source.handle ||
                source.id != capability.owner.id || source.revision != capability.generation) {
                throw std::logic_error("shared source changed after planning");
            }
        }
        for (const OwnerClaim& claim : choice.private_claims_) {
            const std::uint32_t slot = claim.capability.slot;
            if (slot >= catalog_count_ ||
                (choice.private_source_ && slot == choice.private_source_->slot)) {
                throw std::logic_error("private pressure owner is invalid");
            }
            const CatalogEntry& entry = catalog_[slot];
            if (entry.state != CatalogState::Catalogued || !entry.handle ||
                claim.capability.owner.kind != LogicalOwnerKind::PrivateContinuation ||
                entry.id != claim.capability.owner.id ||
                entry.revision != claim.capability.generation || private_has_active_edge(slot) ||
                (claim.disposition != VictimDisposition::Retained &&
                 claim.disposition != VictimDisposition::Evicted)) {
                throw std::logic_error("private pressure owner changed after planning");
            }
        }
        for (const OwnerClaim& claim : choice.shared_claims_) {
            const std::uint32_t slot = claim.capability.slot;
            if (slot >= shared_catalog_count_ ||
                (choice.shared_source_ && slot == choice.shared_source_->slot)) {
                throw std::logic_error("shared pressure owner is invalid");
            }
            const SharedCatalogEntry& entry = shared_catalog_[slot];
            if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                claim.capability.owner.kind != LogicalOwnerKind::SharedPrefix ||
                entry.id != claim.capability.owner.id ||
                entry.revision != claim.capability.generation || entry.transaction_pins != 0 ||
                shared_active_edge_count(slot) != 0 ||
                (claim.disposition != VictimDisposition::Retained &&
                 claim.disposition != VictimDisposition::Evicted)) {
                throw std::logic_error("shared pressure owner changed after planning");
            }
        }
        const CatalogEntry& publication = catalog_[choice.publication_slot_];
        const bool source_cell          = choice.private_source_ &&
                                 choice.publication_slot_ == choice.private_source_->slot &&
                                 choice.source_mode_ == PrivateSourceMode::ConsumeToActive;
        const auto victim =
            std::find_if(choice.private_claims_.begin(), choice.private_claims_.end(),
                         [&](const OwnerClaim& claim) {
                             return claim.capability.slot == choice.publication_slot_;
                         });
        const bool victim_cell = victim != choice.private_claims_.end() &&
                                 victim->disposition == VictimDisposition::Evicted;
        if (publication.state != CatalogState::Vacant && !source_cell && !victim_cell) {
            throw std::logic_error("resource choice has no publication cell");
        }
    }

    [[nodiscard]] MaterializationRecord take_materialization_record(Choice& choice) {
        return MaterializationRecord{
            .destination          = choice.destination_,
            .private_source       = choice.private_source_,
            .source_mode          = choice.source_mode_,
            .shared_source        = choice.shared_source_,
            .publication_slot     = choice.publication_slot_,
            .private_claims       = std::move(choice.private_claims_),
            .shared_claims        = std::move(choice.shared_claims_),
            .selected_observation = choice.selected_observation_,
            .session              = std::move(choice.session_),
            .retention            = choice.retention_,
            .update_session_index = choice.update_session_index_,
            .publication_order    = choice.publication_order_,
            .diagnostics          = choice.diagnostics_,
            .demand               = std::move(choice.demand_),
        };
    }

    void reserve_logical_materialization(const MaterializationRecord& record) noexcept {
        lanes_[record.destination.value] = LogicalLaneState::Materializing;
        if (record.private_source) {
            catalog_[record.private_source->slot].state = CatalogState::Claimed;
        }
        if (record.shared_source) {
            ++shared_catalog_[record.shared_source->slot].transaction_pins;
        }
        for (const OwnerClaim& claim : record.private_claims) {
            catalog_[claim.capability.slot].state = CatalogState::Claimed;
        }
        for (const OwnerClaim& claim : record.shared_claims) {
            shared_catalog_[claim.capability.slot].state = SharedCatalogState::Claimed;
        }
        CatalogEntry& publication = catalog_[record.publication_slot];
        if (publication.state == CatalogState::Vacant) {
            publication.state = CatalogState::Claimed;
        }
    }

    void rollback_logical_materialization(const MaterializationRecord& record) noexcept {
        lanes_[record.destination.value] = LogicalLaneState::Free;
        if (record.private_source) {
            catalog_[record.private_source->slot].state = CatalogState::Catalogued;
        }
        if (record.shared_source) {
            SharedCatalogEntry& source = shared_catalog_[record.shared_source->slot];
            if (source.transaction_pins != 0) { --source.transaction_pins; }
        }
        for (const OwnerClaim& claim : record.private_claims) {
            catalog_[claim.capability.slot].state = CatalogState::Catalogued;
        }
        for (const OwnerClaim& claim : record.shared_claims) {
            shared_catalog_[claim.capability.slot].state = SharedCatalogState::Catalogued;
        }
        CatalogEntry& publication = catalog_[record.publication_slot];
        if (publication.id == 0 && !publication.handle) {
            // The restore path also clears the cell it had to abandon; counted here rather than left dead.
            note_cell_clear(CellClearReason::Rollback);
            publication.state = CatalogState::Vacant;
        }
    }

    void reserve_logical_active_capture(const ActiveCaptureRecord& record) noexcept {
        for (const OwnerClaim& claim : record.private_claims) {
            catalog_[claim.capability.slot].state = CatalogState::Claimed;
        }
        for (const OwnerClaim& claim : record.shared_claims) {
            shared_catalog_[claim.capability.slot].state = SharedCatalogState::Claimed;
        }
        shared_catalog_[record.publication_slot].state = SharedCatalogState::ReservedCapture;
    }

    void rollback_logical_active_capture(const ActiveCaptureRecord& record) noexcept {
        for (const OwnerClaim& claim : record.private_claims) {
            catalog_[claim.capability.slot].state = CatalogState::Catalogued;
        }
        for (const OwnerClaim& claim : record.shared_claims) {
            shared_catalog_[claim.capability.slot].state = SharedCatalogState::Catalogued;
        }
        shared_catalog_[record.publication_slot].state = record.replacement_id == 0
                                                             ? SharedCatalogState::Vacant
                                                             : SharedCatalogState::Catalogued;
    }

    void observe_selected_hit(const MaterializationRecord& record) noexcept {
        if (record.selected_observation) {
            RetentionObservation* selected = resolve_observation(*record.selected_observation);
            if (selected) {
                saturating_increment(selected->selected_hit_count);
                selected->last_hit_epoch = ++retention_epoch_;
            }
        }
    }

    void commit_demand(PrefixDemandRecord&& demand) noexcept {
        if (demand_window_.capacity() < kDemandWindowCapacity) { std::terminate(); }
        if (demand_window_.size() == kDemandWindowCapacity) {
            demand_window_.erase(demand_window_.begin());
        }
        demand_window_.push_back(std::move(demand));
        saturating_increment(demand_epoch_);
        const PrefixDemandRecord& committed = demand_window_.back();
        for (SharedCatalogEntry& entry : shared_catalog_) {
            if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                !entry.explicit_credit) {
                continue;
            }
            if (std::find(committed.exact_resident_keys.begin(),
                          committed.exact_resident_keys.end(),
                          entry.summary.checkpoint.shortlist_key) !=
                committed.exact_resident_keys.end()) {
                entry.explicit_credit     = false;
                entry.credit_expiry_epoch = 0;
                continue;
            }
            if (demand_epoch_ >= entry.credit_expiry_epoch) {
                entry.explicit_credit     = false;
                entry.credit_expiry_epoch = 0;
            }
        }
    }

    void observe_planner_diagnostics(const MaterializationDiagnostics& diagnostics) noexcept {
        if (diagnostics.stop_reason != MaterializationStopReason::NoPressure) {
            saturating_increment(context_stats_.pressure_searches);
        }
        if (diagnostics.budget_exhausted) {
            saturating_increment(context_stats_.pressure_search_budget_exhaustions);
        }
        if (diagnostics.target_arena_truncated) {
            saturating_increment(context_stats_.pressure_target_arena_truncations);
        }
        if (diagnostics.selected_maximal_fallback) {
            saturating_increment(context_stats_.pressure_maximal_fallback_selections);
        }
    }

    [[nodiscard]] RetentionObservation*
    resolve_observation(const PolicyObservationKey& key) noexcept {
        if (!key.shared) {
            if (key.slot >= catalog_count_) { return nullptr; }
            CatalogEntry& entry = catalog_[key.slot];
            if (entry.id != key.owner_id || entry.revision != key.revision) { return nullptr; }
            return find_observation(entry.observations, key.checkpoint);
        }
        if (key.slot >= shared_catalog_count_) { return nullptr; }
        SharedCatalogEntry& entry = shared_catalog_[key.slot];
        if (entry.id != key.owner_id || entry.revision != key.revision ||
            entry.summary.checkpoint.ref != key.checkpoint) {
            return nullptr;
        }
        return &entry.observation;
    }

    [[nodiscard]] static bool continuation_contains_checkpoint(const ContinuationSummary& summary,
                                                               CheckpointRef checkpoint) noexcept {
        if (summary.endpoint && summary.endpoint->ref == checkpoint) { return true; }
        if (summary.rewrite && summary.rewrite->ref == checkpoint) { return true; }
        return std::any_of(summary.long_anchors.begin(), summary.long_anchors.end(),
                           [&](const auto& anchor) { return anchor.ref == checkpoint; });
    }

    [[nodiscard]] static std::uint32_t
    continuation_checkpoint_count(const ContinuationSummary& summary) noexcept {
        const std::size_t count = static_cast<std::size_t>(summary.endpoint.has_value()) +
                                  static_cast<std::size_t>(summary.rewrite.has_value()) +
                                  summary.long_anchors.size();
        return count > std::numeric_limits<std::uint32_t>::max()
                   ? std::numeric_limits<std::uint32_t>::max()
                   : static_cast<std::uint32_t>(count);
    }

    template <class ContainsCheckpoint>
    [[nodiscard]] static std::vector<CheckpointRef> selected_checkpoint_drops(
        PlanningOwnerId owner, VictimDisposition disposition, std::uint32_t expected_drop_count,
        std::span<const PressureCheckpointOutcome> outcomes, std::uint32_t checkpoint_count,
        ContainsCheckpoint&& contains_checkpoint) {
        std::vector<CheckpointRef> dropped;
        dropped.reserve(expected_drop_count);
        std::uint32_t observed = 0;
        for (std::size_t index = 0; index < outcomes.size(); ++index) {
            const PressureCheckpointOutcome& outcome = outcomes[index];
            if (outcome.owner != owner) { continue; }
            if (!contains_checkpoint(outcome.checkpoint) ||
                std::find_if(outcomes.begin(), outcomes.begin() + index,
                             [&](const PressureCheckpointOutcome& prior) {
                                 return prior.owner == owner &&
                                        prior.checkpoint == outcome.checkpoint;
                             }) != outcomes.begin() + index) {
                throw std::logic_error("selected checkpoint outcome is unknown or duplicated");
            }
            ++observed;
            if (!outcome.survives) { dropped.push_back(outcome.checkpoint); }
        }
        if (observed != checkpoint_count || dropped.size() != expected_drop_count ||
            (disposition == VictimDisposition::Evicted) != (dropped.size() == checkpoint_count)) {
            throw std::logic_error("selected checkpoint outcome is incomplete");
        }
        return dropped;
    }

    [[nodiscard]] static bool continuation_matches_claim(
        const ContinuationSummary& before, const std::optional<ContinuationSummary>& after,
        VictimDisposition disposition, std::span<const CheckpointRef> expected_drops) noexcept {
        const std::uint32_t before_count = continuation_checkpoint_count(before);
        if (disposition == VictimDisposition::Evicted) {
            return !after && expected_drops.size() == before_count;
        }
        if (disposition != VictimDisposition::Retained || !after ||
            continuation_checkpoint_count(*after) + expected_drops.size() != before_count) {
            return false;
        }
        const auto expected_drop = [&](CheckpointRef checkpoint) {
            return std::find(expected_drops.begin(), expected_drops.end(), checkpoint) !=
                   expected_drops.end();
        };
        const auto check = [&](CheckpointRef checkpoint) {
            return continuation_contains_checkpoint(*after, checkpoint) !=
                   expected_drop(checkpoint);
        };
        if ((before.endpoint && !check(before.endpoint->ref)) ||
            (before.rewrite && !check(before.rewrite->ref))) {
            return false;
        }
        for (const auto& anchor : before.long_anchors) {
            if (!check(anchor.ref)) { return false; }
        }
        return std::all_of(expected_drops.begin(), expected_drops.end(), [&](CheckpointRef drop) {
            return continuation_contains_checkpoint(before, drop);
        });
    }

    [[nodiscard]] static std::uint32_t
    dropped_checkpoint_count(const ContinuationSummary& before,
                             const std::optional<ContinuationSummary>& after,
                             VictimDisposition disposition) noexcept {
        if (disposition == VictimDisposition::Evicted) {
            return continuation_checkpoint_count(before);
        }
        if (!after) { return 0; }
        std::uint32_t dropped = 0;
        const auto visit      = [&](const auto& checkpoint) {
            if (checkpoint && !continuation_contains_checkpoint(*after, checkpoint->ref)) {
                ++dropped;
            }
        };
        visit(before.endpoint);
        visit(before.rewrite);
        for (const auto& anchor : before.long_anchors) {
            if (!continuation_contains_checkpoint(*after, anchor.ref)) { ++dropped; }
        }
        return dropped;
    }

    static void record_checkpoint_drops(RuntimeStats& stats, std::uint32_t count) noexcept {
        for (std::uint32_t index = 0; index < count; ++index) {
            saturating_increment(stats.pressure_checkpoints_dropped);
        }
    }

    template <class Result>
    void validate_private_action(const OwnerClaim& claim, bool target_committed,
                                 const Result& result) const {
        const std::uint32_t slot = claim.capability.slot;
        if (slot >= catalog_count_) {
            throw std::logic_error("private action result has an invalid slot");
        }
        const CatalogEntry& entry = catalog_[slot];
        if (claim.capability.owner.kind != LogicalOwnerKind::PrivateContinuation ||
            entry.state != CatalogState::Claimed || entry.id != claim.capability.owner.id ||
            entry.revision != claim.capability.generation || !entry.handle) {
            throw std::logic_error("private action owner changed before adoption");
        }
        if (result.final_summary && !valid_continuation_summary(*result.final_summary)) {
            throw std::logic_error("private action returned an invalid final summary");
        }
        const std::uint32_t dropped =
            dropped_checkpoint_count(entry.summary, result.final_summary, result.disposition);
        if (target_committed && !result.pressure_committed) {
            throw std::logic_error("selected private pressure action was not committed");
        }
        if (result.pressure_committed &&
            (result.disposition != claim.disposition ||
             !continuation_matches_claim(entry.summary, result.final_summary, result.disposition,
                                         claim.dropped_checkpoints))) {
            throw std::logic_error("private owner outcome differs from the selected target");
        }
        if (result.disposition == VictimDisposition::Evicted) {
            if (!result.pressure_committed || result.final_summary) {
                throw std::logic_error("private eviction result is malformed");
            }
            return;
        }
        if (result.disposition != VictimDisposition::Retained) {
            throw std::logic_error("private pressure action returned an invalid disposition");
        }
        if (!result.pressure_committed &&
            (result.disposition != VictimDisposition::Retained || dropped != 0 ||
             (result.final_summary &&
              !continuation_matches_claim(entry.summary, result.final_summary,
                                          VictimDisposition::Retained, {})))) {
            throw std::logic_error("uncommitted private pressure action changed checkpoints");
        }
        if (result.pressure_committed && !result.final_summary) {
            throw std::logic_error("committed private pressure action has no final summary");
        }
    }

    template <class Result>
    void validate_shared_action(const OwnerClaim& claim, bool target_committed,
                                const Result& result) const {
        const std::uint32_t slot = claim.capability.slot;
        if (slot >= shared_catalog_count_) {
            throw std::logic_error("shared action result has an invalid slot");
        }
        const SharedCatalogEntry& entry = shared_catalog_[slot];
        if (claim.capability.owner.kind != LogicalOwnerKind::SharedPrefix ||
            entry.state != SharedCatalogState::Claimed || entry.id != claim.capability.owner.id ||
            entry.revision != claim.capability.generation || !entry.handle) {
            throw std::logic_error("shared action owner changed before adoption");
        }
        if (result.final_summary && !valid_shared_prefix_summary(*result.final_summary)) {
            throw std::logic_error("shared action returned an invalid final summary");
        }
        if (target_committed && !result.pressure_committed) {
            throw std::logic_error("selected shared pressure action was not committed");
        }
        const bool exact_committed_outcome =
            result.disposition == VictimDisposition::Evicted
                ? claim.dropped_checkpoints.size() == 1U &&
                      claim.dropped_checkpoints.front() == entry.summary.checkpoint.ref &&
                      !result.final_summary
                : result.disposition == VictimDisposition::Retained &&
                      claim.dropped_checkpoints.empty() && result.final_summary &&
                      result.final_summary->checkpoint.ref == entry.summary.checkpoint.ref;
        if (result.pressure_committed &&
            (result.disposition != claim.disposition || !exact_committed_outcome)) {
            throw std::logic_error("shared owner outcome differs from the selected target");
        }
        if (result.disposition == VictimDisposition::Evicted) {
            if (!result.pressure_committed || result.final_summary) {
                throw std::logic_error("shared eviction result is malformed");
            }
            return;
        }
        if (result.disposition != VictimDisposition::Retained) {
            throw std::logic_error("shared pressure action returned an invalid disposition");
        }
        if (!result.pressure_committed && result.final_summary &&
            result.final_summary->checkpoint.ref != entry.summary.checkpoint.ref) {
            throw std::logic_error(
                "uncommitted shared pressure action changed checkpoint identity");
        }
        if (result.pressure_committed && !result.final_summary) {
            throw std::logic_error("committed shared pressure action has no final summary");
        }
    }

    template <class Result>
    void apply_private_action(const OwnerClaim& claim, bool target_committed,
                              const Result& result) noexcept {
        (void)target_committed;
        const std::uint32_t slot = claim.capability.slot;
        CatalogEntry& entry      = catalog_[slot];
        const std::uint32_t dropped =
            dropped_checkpoint_count(entry.summary, result.final_summary, result.disposition);
        if (result.disposition == VictimDisposition::Evicted) {
            erase_session_if_owner(claim.capability.owner.id, SessionEraseReason::Eviction);
            clear_catalog_entry(entry);
            // THE CELL CLEAR ON THE PRESSURE PATH. This increment lived only in `clear_after_program_cleanup`,
            // beside Cleanup, so `catalog_cell_clears_action` read 0 no matter how many owners were evicted --
            // and a reader comparing it against `pressure_private_owners_evicted` (live: 65) read the
            // miswiring as the finding "evictions do not clear cells". They do; it was not counted. The
            // previous comment here explained the gap as "different routes", which was invented.
            note_cell_clear(CellClearReason::Action);
            saturating_increment(context_stats_.pressure_private_owners_evicted);
            record_checkpoint_drops(context_stats_, dropped);
            return;
        }
        if (!result.pressure_committed) {
            entry.state = CatalogState::Catalogued;
            return;
        }
        assign_continuation_summary(entry.summary, *result.final_summary);
        migrate_observations(entry, *result.final_summary, entry.retention);
        advance_revision(entry.revision);
        refresh_session_owner_revision(claim.capability.owner.id, slot, entry.revision);
        saturating_increment(context_stats_.pressure_private_owners_degraded);
        // A committed non-evicted action that leaves the endpoint resident on host (HostOnly /
        // Both) is a demote-to-host: the checkpoint stays Catalogued + session-cell (restorable)
        // rather than dropped. Track it separately from a plain in-device degrade.
        const auto& final_summary = *result.final_summary;
        const bool state_hosted =
            final_summary.endpoint &&
            (final_summary.endpoint->state_residency == runtime::ReplicaResidency::HostOnly ||
             final_summary.endpoint->state_residency == runtime::ReplicaResidency::Both);
        if (state_hosted) {
            saturating_increment(context_stats_.pressure_private_owners_demoted);
        }
        // The KV axis of a demote is NOT countable here: `MaterializationVictimResult` carries no
        // operation counts and `CheckpointSummary` carries no KV residency, so the only place the
        // information exists is the pressure work that performs the spill
        // (publish_pressure_work -> ContextOperationCounts, folded into context_stats_ by
        // observe_operations). An attempt to count it here did not compile, which is the honest reason
        // it lives there instead.
        record_checkpoint_drops(context_stats_, dropped);
        entry.state = CatalogState::Catalogued;
    }

    template <class Result>
    void apply_shared_action(const OwnerClaim& claim, bool target_committed,
                             const Result& result) noexcept {
        (void)target_committed;
        const std::uint32_t slot    = claim.capability.slot;
        SharedCatalogEntry& entry   = shared_catalog_[slot];
        const std::uint32_t dropped = result.disposition == VictimDisposition::Evicted ? 1U : 0U;
        if (result.disposition == VictimDisposition::Evicted) {
            clear_shared_entry(entry);
            saturating_increment(context_stats_.pressure_shared_owners_evicted);
            record_checkpoint_drops(context_stats_, dropped);
            return;
        }
        if (!result.pressure_committed) {
            entry.state = SharedCatalogState::Catalogued;
            return;
        }
        entry.summary = *result.final_summary;
        advance_revision(entry.revision);
        saturating_increment(context_stats_.pressure_shared_owners_degraded);
        record_checkpoint_drops(context_stats_, dropped);
        entry.state = SharedCatalogState::Catalogued;
    }

    void restore_unreported_materialization(const MaterializationRecord& record) noexcept {
        if (record.private_source &&
            catalog_[record.private_source->slot].state == CatalogState::Claimed) {
            catalog_[record.private_source->slot].state = CatalogState::Catalogued;
        }
        if (record.shared_source) {
            SharedCatalogEntry& source = shared_catalog_[record.shared_source->slot];
            if (source.transaction_pins != 0) { --source.transaction_pins; }
        }
        for (const OwnerClaim& claim : record.private_claims) {
            CatalogEntry& entry = catalog_[claim.capability.slot];
            if (entry.state == CatalogState::Claimed) { entry.state = CatalogState::Catalogued; }
        }
        for (const OwnerClaim& claim : record.shared_claims) {
            SharedCatalogEntry& entry = shared_catalog_[claim.capability.slot];
            if (entry.state == SharedCatalogState::Claimed) {
                entry.state = SharedCatalogState::Catalogued;
            }
        }
        CatalogEntry& publication = catalog_[record.publication_slot];
        if (publication.state == CatalogState::Claimed && publication.id == 0 &&
            !publication.handle) {
            // The restore path also clears the cell it had to abandon; counted here rather than left dead.
            note_cell_clear(CellClearReason::Rollback);
            publication.state = CatalogState::Vacant;
        }
    }

    [[nodiscard]] MaterializationOutcome
    adopt_materialization_progress(Program& program, ProgramMaterializationResult&& result) {
        MaterializationRecord* record = std::get_if<MaterializationRecord>(&transaction_);
        if (record == nullptr || !program.has_context_transaction()) {
            throw std::logic_error("materialization result has no logical transaction");
        }
        if (result.status == ContextTransactionStatus::InProgress) {
            throw std::logic_error("terminal materialization result is marked in progress");
        }
        if (result.victims.size() != record->private_claims.size() ||
            result.shared_victims.size() != record->shared_claims.size()) {
            throw std::logic_error("materialization result is not action aligned");
        }
        const auto private_result_for = [&](const OwnerClaim& claim) -> const auto& {
            const auto found =
                std::find_if(result.victims.begin(), result.victims.end(),
                             [&](const auto& row) { return row.owner == claim.planning_id; });
            if (found == result.victims.end() ||
                std::find_if(found + 1, result.victims.end(), [&](const auto& row) {
                    return row.owner == claim.planning_id;
                }) != result.victims.end()) {
                throw std::logic_error("materialization private result ID is not unique");
            }
            return *found;
        };
        const auto shared_result_for = [&](const OwnerClaim& claim) -> const auto& {
            const auto found =
                std::find_if(result.shared_victims.begin(), result.shared_victims.end(),
                             [&](const auto& row) { return row.owner == claim.planning_id; });
            if (found == result.shared_victims.end() ||
                std::find_if(found + 1, result.shared_victims.end(), [&](const auto& row) {
                    return row.owner == claim.planning_id;
                }) != result.shared_victims.end()) {
                throw std::logic_error("materialization shared result ID is not unique");
            }
            return *found;
        };

        const bool published = result.status == ContextTransactionStatus::Published;
        if ((!published && result.status != ContextTransactionStatus::Aborted) ||
            published != result.published.has_value()) {
            throw std::logic_error("materialization terminal status is invalid");
        }
        if (record->destination.value >= lane_count_ ||
            lanes_[record->destination.value] != LogicalLaneState::Materializing ||
            active_[record->destination.value].occupied ||
            active_[record->destination.value].retained_private_source ||
            !active_[record->destination.value].shared_sources.empty() ||
            active_[record->destination.value].shared_sources.capacity() < shared_catalog_count_ ||
            record->publication_slot >= catalog_count_ ||
            catalog_[record->publication_slot].state != CatalogState::Claimed) {
            throw std::logic_error("materialization logical destination changed before adoption");
        }
        for (std::size_t row = 0; row < record->private_claims.size(); ++row) {
            const OwnerClaim& claim = record->private_claims[row];
            if ((record->private_source && claim.capability.slot == record->private_source->slot) ||
                std::find_if(record->private_claims.begin(),
                             record->private_claims.begin() + static_cast<std::ptrdiff_t>(row),
                             [&](const OwnerClaim& prior) {
                                 return prior.planning_id == claim.planning_id ||
                                        prior.capability == claim.capability;
                             }) !=
                    record->private_claims.begin() + static_cast<std::ptrdiff_t>(row)) {
                throw std::logic_error("materialization private claim manifest is not unique");
            }
        }
        for (std::size_t row = 0; row < record->shared_claims.size(); ++row) {
            const OwnerClaim& claim = record->shared_claims[row];
            if (std::any_of(record->private_claims.begin(), record->private_claims.end(),
                            [&](const OwnerClaim& prior) {
                                return prior.planning_id == claim.planning_id;
                            })) {
                throw std::logic_error("materialization owner ID changes kind");
            }
            if ((record->shared_source && claim.capability.slot == record->shared_source->slot) ||
                std::find_if(record->shared_claims.begin(),
                             record->shared_claims.begin() + static_cast<std::ptrdiff_t>(row),
                             [&](const OwnerClaim& prior) {
                                 return prior.planning_id == claim.planning_id ||
                                        prior.capability == claim.capability;
                             }) !=
                    record->shared_claims.begin() + static_cast<std::ptrdiff_t>(row)) {
                throw std::logic_error("materialization shared claim manifest is not unique");
            }
        }
        for (const OwnerClaim& claim : record->private_claims) {
            validate_private_action(claim, published, private_result_for(claim));
        }
        for (const OwnerClaim& claim : record->shared_claims) {
            validate_shared_action(claim, published, shared_result_for(claim));
        }

        if (record->private_source) {
            const CatalogCapability& capability = *record->private_source;
            if (capability.owner.kind != LogicalOwnerKind::PrivateContinuation ||
                capability.slot >= catalog_count_) {
                throw std::logic_error("materialization private source capability is malformed");
            }
            const CatalogEntry& source = catalog_[capability.slot];
            if (!result.source || source.state != CatalogState::Claimed ||
                source.id != capability.owner.id || source.revision != capability.generation ||
                (!published && result.source->mode != PrivateSourceMode::Retain) ||
                (published && result.source->mode != record->source_mode) ||
                (result.source->final_summary &&
                 !valid_continuation_summary(*result.source->final_summary)) ||
                (result.source->mode == PrivateSourceMode::ConsumeToActive &&
                 result.source->final_summary) ||
                (published && result.source->mode == PrivateSourceMode::Retain &&
                 private_has_active_edge(capability.slot))) {
                throw std::logic_error("materialization private source result is invalid");
            }
        } else if (result.source) {
            throw std::logic_error("root materialization returned a private source result");
        }

        if (record->shared_source) {
            const CatalogCapability& capability = *record->shared_source;
            if (capability.owner.kind != LogicalOwnerKind::SharedPrefix ||
                capability.slot >= shared_catalog_count_) {
                throw std::logic_error("materialization shared source capability is malformed");
            }
            const SharedCatalogEntry& source       = shared_catalog_[capability.slot];
            const std::uint32_t current_references = shared_active_edge_count(capability.slot);
            if (!result.shared_source || source.transaction_pins == 0 ||
                source.id != capability.owner.id || source.revision != capability.generation ||
                current_references == std::numeric_limits<std::uint32_t>::max()) {
                throw std::logic_error("materialization shared source result is invalid");
            }
            const std::uint32_t expected_references =
                published ? current_references + 1U : current_references;
            if (result.shared_source->final_summary) {
                const SharedPrefixSummary& final = *result.shared_source->final_summary;
                SharedPrefixSummary expected     = source.summary;
                expected.active_references       = expected_references;
                // Source reuse may restore a replica, but it cannot rewrite the checkpoint
                // identity or its recovery contract.
                expected.checkpoint.state_residency = final.checkpoint.state_residency;
                if (!valid_shared_prefix_summary(final) || final != expected) {
                    throw std::logic_error("materialization shared source summary is invalid");
                }
            }
        } else if (result.shared_source) {
            throw std::logic_error("materialization returned an unexpected shared source result");
        }

        if (published) {
            const CatalogEntry& publication = catalog_[record->publication_slot];
            bool publication_released       = !publication.handle && publication.id == 0;
            publication_released =
                publication_released ||
                (record->private_source &&
                 record->publication_slot == record->private_source->slot && result.source &&
                 result.source->mode == PrivateSourceMode::ConsumeToActive);
            for (const OwnerClaim& claim : record->private_claims) {
                if (publication_released || claim.capability.slot != record->publication_slot) {
                    continue;
                }
                const auto& victim = private_result_for(claim);
                publication_released =
                    victim.disposition == VictimDisposition::Evicted && victim.pressure_committed;
            }
            if (!publication_released) {
                throw std::logic_error(
                    "materialization result cannot release its publication cell");
            }
        }

        if (published) { observe_selected_hit(*record); }
        for (const OwnerClaim& claim : record->private_claims) {
            apply_private_action(claim, published, private_result_for(claim));
        }
        for (const OwnerClaim& claim : record->shared_claims) {
            apply_shared_action(claim, published, shared_result_for(claim));
        }

        bool retained_private_source = false;
        if (record->private_source) {
            const CatalogCapability capability = *record->private_source;
            CatalogEntry& source               = catalog_[capability.slot];
            if (result.source->mode == PrivateSourceMode::Retain) {
                if (result.source->final_summary) {
                    assign_continuation_summary(source.summary, *result.source->final_summary);
                    migrate_observations(source, *result.source->final_summary, source.retention);
                    advance_revision(source.revision);
                    refresh_session_owner_revision(capability.owner.id, capability.slot,
                                                   source.revision);
                }
                source.state            = CatalogState::Catalogued;
                retained_private_source = result.status == ContextTransactionStatus::Published;
            } else if (result.source->mode == PrivateSourceMode::ConsumeToActive) {
                erase_session_if_owner(source.id, SessionEraseReason::Consume);
                source.handle.reset();
                source.summary.endpoint.reset();
                source.summary.rewrite.reset();
                source.summary.long_anchors.clear();
                source.observations.clear();
                source.session.reset();
            }
        }

        if (record->shared_source) {
            SharedCatalogEntry& source = shared_catalog_[record->shared_source->slot];
            --source.transaction_pins;
            if (result.shared_source->final_summary) {
                SharedPrefixSummary updated = *result.shared_source->final_summary;
                updated.active_references   = 0;
                if (updated != source.summary) {
                    source.summary = std::move(updated);
                    advance_revision(source.revision);
                }
            }
        }

        observe_transfers(result);
        observe_operations(result);

        if (result.status == ContextTransactionStatus::Aborted) {
            restore_unreported_materialization(*record);
            lanes_[record->destination.value] = LogicalLaneState::Free;
            transaction_.template emplace<std::monostate>();
            program.finalize_context_transaction();
            return {.status = ContextTransactionStatus::Aborted};
        }

        CatalogEntry& publication = catalog_[record->publication_slot];
        publication.state         = CatalogState::ReservedForActive;
        publication.id            = next_continuation_id_++;
        publication.session       = record->session;
        publication.retention     = record->retention;
        advance_revision(publication.revision);
        if (publication.id == 0) { publication.id = next_continuation_id_++; }

        ActiveEntry& active         = active_[record->destination.value];
        active.occupied             = true;
        active.publication_slot     = record->publication_slot;
        active.continuation_id      = publication.id;
        active.session              = record->session;
        active.retention            = record->retention;
        active.update_session_index = record->update_session_index;
        active.publication_order    = record->publication_order;
        if (retained_private_source) {
            active.retained_private_source =
                active_edge(private_capability(record->private_source->slot));
        }
        if (record->shared_source) {
            active.shared_sources.push_back(
                active_edge(shared_capability(record->shared_source->slot)));
        }
        StartResult start = std::move(*result.published);
        result.published.reset();
        commit_demand(std::move(record->demand));
        return MaterializationOutcome{
            .status      = ContextTransactionStatus::Published,
            .activation  = PublishedActivation(*this, std::move(start), record->destination),
            .diagnostics = record->diagnostics,
        };
    }

    [[nodiscard]] ActiveCaptureOutcome
    adopt_active_capture_progress(Program& program, ProgramActiveCaptureResult&& result) {
        ActiveCaptureRecord* record = std::get_if<ActiveCaptureRecord>(&transaction_);
        if (record == nullptr || !program.has_context_transaction()) {
            throw std::logic_error("active capture result has no logical transaction");
        }
        if (result.status == ContextTransactionStatus::InProgress) {
            throw std::logic_error("terminal capture result is marked in progress");
        }
        if (result.victims.size() != record->private_claims.size() ||
            result.shared_victims.size() != record->shared_claims.size()) {
            throw std::logic_error("active capture result is not pressure-action aligned");
        }
        const auto private_result_for = [&](const OwnerClaim& claim) -> const auto& {
            const auto found =
                std::find_if(result.victims.begin(), result.victims.end(),
                             [&](const auto& row) { return row.owner == claim.planning_id; });
            if (found == result.victims.end() ||
                std::find_if(found + 1, result.victims.end(), [&](const auto& row) {
                    return row.owner == claim.planning_id;
                }) != result.victims.end()) {
                throw std::logic_error("active capture private result ID is not unique");
            }
            return *found;
        };
        const auto shared_result_for = [&](const OwnerClaim& claim) -> const auto& {
            const auto found =
                std::find_if(result.shared_victims.begin(), result.shared_victims.end(),
                             [&](const auto& row) { return row.owner == claim.planning_id; });
            if (found == result.shared_victims.end() ||
                std::find_if(found + 1, result.shared_victims.end(), [&](const auto& row) {
                    return row.owner == claim.planning_id;
                }) != result.shared_victims.end()) {
                throw std::logic_error("active capture shared result ID is not unique");
            }
            return *found;
        };
        const bool published = result.status == ContextTransactionStatus::Published;
        if (!published && result.status != ContextTransactionStatus::Aborted) {
            throw std::logic_error("active capture terminal status is invalid");
        }
        for (std::size_t row = 0; row < record->private_claims.size(); ++row) {
            const OwnerClaim& claim = record->private_claims[row];
            if (std::find_if(record->private_claims.begin(),
                             record->private_claims.begin() + static_cast<std::ptrdiff_t>(row),
                             [&](const OwnerClaim& prior) {
                                 return prior.planning_id == claim.planning_id ||
                                        prior.capability == claim.capability;
                             }) !=
                record->private_claims.begin() + static_cast<std::ptrdiff_t>(row)) {
                throw std::logic_error("active capture private claim manifest is not unique");
            }
        }
        for (std::size_t row = 0; row < record->shared_claims.size(); ++row) {
            const OwnerClaim& claim = record->shared_claims[row];
            if (std::any_of(record->private_claims.begin(), record->private_claims.end(),
                            [&](const OwnerClaim& prior) {
                                return prior.planning_id == claim.planning_id;
                            })) {
                throw std::logic_error("active capture owner ID changes kind");
            }
            if (claim.capability.slot == record->publication_slot ||
                std::find_if(record->shared_claims.begin(),
                             record->shared_claims.begin() + static_cast<std::ptrdiff_t>(row),
                             [&](const OwnerClaim& prior) {
                                 return prior.planning_id == claim.planning_id ||
                                        prior.capability == claim.capability;
                             }) !=
                    record->shared_claims.begin() + static_cast<std::ptrdiff_t>(row)) {
                throw std::logic_error("active capture shared claim manifest is not unique");
            }
        }
        for (const OwnerClaim& claim : record->private_claims) {
            validate_private_action(claim, published, private_result_for(claim));
        }
        for (const OwnerClaim& claim : record->shared_claims) {
            validate_shared_action(claim, published, shared_result_for(claim));
        }
        if (record->lane.value >= lane_count_ || !active_[record->lane.value].occupied ||
            lanes_[record->lane.value] != LogicalLaneState::Active) {
            throw std::logic_error("active capture owner left its lane");
        }
        if (record->publishes_shared) {
            if (record->publication_slot >= shared_catalog_count_) {
                throw std::logic_error("active capture shared publication slot is invalid");
            }
            const SharedCatalogEntry& publication = shared_catalog_[record->publication_slot];
            if (publication.state != SharedCatalogState::ReservedCapture ||
                shared_active_edge_count(record->publication_slot) != 0 ||
                (record->replacement_id == 0 && (publication.id != 0 || publication.handle ||
                                                 result.capacity_preparation_committed)) ||
                (record->replacement_id != 0 &&
                 (publication.id != record->replacement_id ||
                  publication.revision != record->replacement_revision || !publication.handle ||
                  (published && !result.capacity_preparation_committed)))) {
                throw std::logic_error("active capture publication changed before adoption");
            }
        } else if (record->publication_slot != kInvalidCatalogSlot || record->replacement_id != 0 ||
                   result.capacity_preparation_committed) {
            throw std::logic_error("private capture has shared publication state");
        }
        if (!published) {
            if (result.shared) {
                throw std::logic_error("aborted active capture published a shared prefix");
            }
        } else {
            const ActiveEntry& active = active_[record->lane.value];
            if (record->publishes_private) {
                if (!valid_continuation_summary(result.active_summary)) {
                    throw std::logic_error("active capture returned an invalid private summary");
                }
                const CatalogEntry& publication = catalog_.at(active.publication_slot);
                if (publication.state != CatalogState::ReservedForActive ||
                    publication.id != active.continuation_id) {
                    throw std::logic_error("active private publication changed during capture");
                }
            }
            if (record->publishes_shared) {
                if (!result.shared ||
                    active.shared_sources.size() == active.shared_sources.capacity() ||
                    !valid_shared_prefix_summary(result.shared->summary) ||
                    result.shared->summary.active_references != 1) {
                    throw std::logic_error("active capture returned an invalid shared publication");
                }
            } else if (result.shared) {
                throw std::logic_error("private capture returned an unexpected shared publication");
            }
        }
        for (const OwnerClaim& claim : record->private_claims) {
            apply_private_action(claim, published, private_result_for(claim));
        }
        for (const OwnerClaim& claim : record->shared_claims) {
            apply_shared_action(claim, published, shared_result_for(claim));
        }
        if (result.status == ContextTransactionStatus::Aborted) {
            if (record->publication_slot != kInvalidCatalogSlot) {
                SharedCatalogEntry& publication = shared_catalog_[record->publication_slot];
                if (record->replacement_id != 0 && result.capacity_preparation_committed) {
                    clear_shared_entry(publication);
                } else if (record->replacement_id != 0) {
                    publication.state = SharedCatalogState::Catalogued;
                } else {
                    clear_shared_entry(publication);
                }
            }
            observe_transfers(result);
            observe_operations(result);
            transaction_.template emplace<std::monostate>();
            program.finalize_context_transaction();
            return {.status = ContextTransactionStatus::Aborted};
        }
        ActiveEntry& active = active_[record->lane.value];
        if (record->publishes_private) {
            CatalogEntry& publication = catalog_[active.publication_slot];
            assign_continuation_summary(publication.summary, result.active_summary);
            migrate_observations(publication, result.active_summary, active.retention);
            advance_revision(publication.revision);
        }
        if (record->publishes_shared) {
            SharedCatalogEntry& publication = shared_catalog_[record->publication_slot];
            publication.handle.reset();
            publication.state = SharedCatalogState::Catalogued;
            publication.id    = next_shared_prefix_id_++;
            if (publication.id == 0) { publication.id = next_shared_prefix_id_++; }
            publication.summary                   = result.shared->summary;
            publication.summary.active_references = 0;
            publication.handle.emplace(std::move(result.shared->handle));
            publication.observation =
                RetentionObservation{.retention_class = RetentionClass::SharedStable};
            publication.transaction_pins = 0;
            publication.explicit_credit =
                has_shared_candidate_evidence(record->shared_evidence,
                                              SharedCandidateEvidence::ExplicitBoundary) ||
                has_shared_candidate_evidence(record->shared_evidence,
                                              SharedCandidateEvidence::RequestedAutomatic);
            publication.credit_expiry_epoch =
                publication.explicit_credit
                    ? (demand_epoch_ >
                               std::numeric_limits<std::uint64_t>::max() - kDemandWindowCapacity
                           ? std::numeric_limits<std::uint64_t>::max()
                           : demand_epoch_ + kDemandWindowCapacity)
                    : 0;
            advance_revision(publication.revision);
            active.shared_sources.push_back(
                active_edge(shared_capability(record->publication_slot)));
        }
        observe_transfers(result);
        observe_operations(result);
        transaction_.template emplace<std::monostate>();
        program.finalize_context_transaction();
        return {.status = ContextTransactionStatus::Published};
    }

    void release_active_references(LaneId lane) {
        ActiveEntry& active = active_[lane.value];
        if (active.retained_private_source) {
            const ActiveOwnerEdge& edge = *active.retained_private_source;
            if (edge.owner.kind != LogicalOwnerKind::PrivateContinuation ||
                edge.slot >= catalog_count_) {
                throw std::logic_error("retained private source edge is malformed");
            }
            const CatalogEntry& source = catalog_[edge.slot];
            if (source.state != CatalogState::Catalogued || !source.handle ||
                source.id != edge.owner.id) {
                throw std::logic_error("retained private source edge is stale");
            }
        }
        for (std::size_t index = 0; index < active.shared_sources.size(); ++index) {
            const ActiveOwnerEdge& edge = active.shared_sources[index];
            if (edge.owner.kind != LogicalOwnerKind::SharedPrefix ||
                edge.slot >= shared_catalog_count_ ||
                std::find(active.shared_sources.begin(), active.shared_sources.begin() + index,
                          edge) != active.shared_sources.begin() + index) {
                throw std::logic_error("shared source edge is malformed");
            }
            const SharedCatalogEntry& source = shared_catalog_[edge.slot];
            if (source.state != SharedCatalogState::Catalogued || !source.handle ||
                source.id != edge.owner.id) {
                throw std::logic_error("shared source edge is stale");
            }
        }
        active.retained_private_source.reset();
        active.shared_sources.clear();
    }

    void release_cancelled_lane(LaneId lane) {
        if (lane.value >= lane_count_ || !active_[lane.value].occupied ||
            (lanes_[lane.value] != LogicalLaneState::Active &&
             lanes_[lane.value] != LogicalLaneState::TerminalPending)) {
            throw std::logic_error("cancelled lane has no logical active owner");
        }
        release_active_references(lane);
        note_cell_clear(CellClearReason::Cancelled);
    clear_catalog_entry(catalog_.at(active_[lane.value].publication_slot));
        reset_active_entry(active_[lane.value]);
        lanes_[lane.value] = LogicalLaneState::Free;
    }

    [[nodiscard]] static std::uint64_t session_hash(const CacheSessionKey& key) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char value : key.view()) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    [[nodiscard]] std::optional<std::size_t>
    find_session_cell(const CacheSessionKey& key) const noexcept {
        if (session_index_.empty()) { return std::nullopt; }
        const std::size_t begin = session_hash(key) % session_index_.size();
        for (std::size_t probe = 0; probe < session_index_.size(); ++probe) {
            const std::size_t cell         = (begin + probe) % session_index_.size();
            const SessionIndexEntry& entry = session_index_[cell];
            if (entry.state == SessionIndexState::Empty) { return std::nullopt; }
            if (entry.state == SessionIndexState::Occupied && entry.key == key) { return cell; }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t session_insert_cell(const CacheSessionKey& key) const {
        if (session_index_.empty()) {
            throw std::logic_error("session publication has no index capacity");
        }
        const std::size_t begin = session_hash(key) % session_index_.size();
        std::optional<std::size_t> deleted;
        for (std::size_t probe = 0; probe < session_index_.size(); ++probe) {
            const std::size_t cell         = (begin + probe) % session_index_.size();
            const SessionIndexEntry& entry = session_index_[cell];
            if (entry.state == SessionIndexState::Occupied && entry.key == key) { return cell; }
            if (entry.state == SessionIndexState::Deleted && !deleted) { deleted = cell; }
            if (entry.state == SessionIndexState::Empty) { return deleted.value_or(cell); }
        }
        if (deleted) { return *deleted; }
        throw std::logic_error("session index is full");
    }

    void erase_session_if_owner(std::uint64_t owner_id, SessionEraseReason reason) noexcept {
        if (owner_id == 0) { return; }
        for (SessionIndexEntry& entry : session_index_) {
            if (entry.state == SessionIndexState::Occupied && entry.owner_id == owner_id) {
                entry.state             = SessionIndexState::Deleted;
                entry.slot              = kInvalidCatalogSlot;
                entry.owner_id          = 0;
                entry.revision          = 0;
                entry.publication_order = 0;
                ++session_erases_[static_cast<std::size_t>(reason)];
            }
        }
    }

    void refresh_session_owner_revision(std::uint64_t owner_id, std::uint32_t slot,
                                        std::uint64_t revision) noexcept {
        if (owner_id == 0 || slot >= catalog_count_) { return; }
        for (SessionIndexEntry& entry : session_index_) {
            if (entry.state == SessionIndexState::Occupied && entry.owner_id == owner_id &&
                entry.slot == slot) {
                entry.revision = revision;
                return;
            }
        }
    }

    [[nodiscard]] bool publish_session(const CacheSessionKey& key, std::uint32_t slot,
                                       std::uint64_t owner_id, std::uint64_t revision,
                                       std::uint64_t publication_order) {
        const std::size_t cell   = session_insert_cell(key);
        SessionIndexEntry& entry = session_index_[cell];
        std::optional<SessionIndexEntry> previous;
        if (entry.state == SessionIndexState::Occupied) {
            if (entry.publication_order > publication_order) { return false; }
            if (entry.publication_order == publication_order) {
                if (entry.slot != slot || entry.owner_id != owner_id) {
                    throw std::logic_error("equal publication order names two continuations");
                }
                entry.revision = revision;
                return true;
            }
            previous = entry;
        }
        entry = SessionIndexEntry{
            .state             = SessionIndexState::Occupied,
            .key               = key,
            .slot              = slot,
            .owner_id          = owner_id,
            .revision          = revision,
            .publication_order = publication_order,
        };
        if (previous && (previous->slot != slot || previous->owner_id != owner_id)) {
            demote_replaced_session(*previous, slot, owner_id);
        }
        return true;
    }

    void demote_replaced_session(const SessionIndexEntry& previous, std::uint32_t replacement_slot,
                                 std::uint64_t replacement_id) noexcept {
        if (previous.slot >= catalog_count_ ||
            (previous.slot == replacement_slot && previous.owner_id == replacement_id)) {
            return;
        }
        CatalogEntry& prior = catalog_[previous.slot];
        if (prior.state != CatalogState::Catalogued || !prior.handle ||
            prior.id != previous.owner_id || prior.revision != previous.revision) {
            return;
        }
        prior.session.reset();
        prior.retention = RetentionClass::RecentPrivate;
        for (CheckpointObservation& observation : prior.observations) {
            observation.observation.retention_class = RetentionClass::RecentPrivate;
        }
    }

    void observe_transfer(const ContextTransferObservation& observation) noexcept {
        const double seconds = static_cast<double>(observation.elapsed_ns) * 1.0e-9;
        context_stats_.actual_context_transfer_seconds += seconds;
        const std::uint64_t bytes = observation.units;
        // Per-request attribution: monotonic, read as a delta.  Both resource classes
        // (State and KV) count -- a restore moves the state image AND the KV it covers,
        // and the request paid for both.
        if (observation.direction == ContextTransferDirection::HostToDevice) {
            transfer_totals_.host_to_device_ns += observation.elapsed_ns;
            transfer_totals_.host_to_device_pages += observation.page_count;
            transfer_totals_.host_to_device_bytes += bytes;
        } else if (observation.direction == ContextTransferDirection::DeviceToHost) {
            transfer_totals_.device_to_host_ns += observation.elapsed_ns;
            transfer_totals_.device_to_host_pages += observation.page_count;
            transfer_totals_.device_to_host_bytes += bytes;
        }
        switch (observation.resource) {
        case ContextResourceClass::State:
            switch (observation.direction) {
            case ContextTransferDirection::DeviceToHost:
                ++context_stats_.state_d2h_count;
                context_stats_.state_d2h_bytes += bytes;
                context_stats_.state_d2h_seconds += seconds;
                break;
            case ContextTransferDirection::HostToDevice:
                ++context_stats_.state_h2d_count;
                context_stats_.state_h2d_bytes += bytes;
                context_stats_.state_h2d_seconds += seconds;
                break;
            case ContextTransferDirection::DeviceToDevice:
                ++context_stats_.state_d2d_count;
                context_stats_.state_d2d_bytes += bytes;
                context_stats_.state_d2d_seconds += seconds;
                break;
            }
            break;
        case ContextResourceClass::MainKV:
            observe_kv_transfer(
                observation, context_stats_.main_kv_d2h_pages, context_stats_.main_kv_h2d_pages,
                context_stats_.main_kv_d2d_pages, context_stats_.main_kv_d2h_bytes,
                context_stats_.main_kv_h2d_bytes, context_stats_.main_kv_d2d_bytes,
                context_stats_.main_kv_d2h_seconds, context_stats_.main_kv_h2d_seconds,
                context_stats_.main_kv_d2d_seconds);
            break;
        case ContextResourceClass::BackendKV:
            observe_kv_transfer(
                observation, context_stats_.backend_kv_d2h_pages,
                context_stats_.backend_kv_h2d_pages, context_stats_.backend_kv_d2d_pages,
                context_stats_.backend_kv_d2h_bytes, context_stats_.backend_kv_h2d_bytes,
                context_stats_.backend_kv_d2d_bytes, context_stats_.backend_kv_d2h_seconds,
                context_stats_.backend_kv_h2d_seconds, context_stats_.backend_kv_d2d_seconds);
            break;
        }
    }

    static void observe_kv_transfer(const ContextTransferObservation& observation,
                                    std::uint64_t& d2h_pages, std::uint64_t& h2d_pages,
                                    std::uint64_t& d2d_pages, std::uint64_t& d2h_bytes,
                                    std::uint64_t& h2d_bytes, std::uint64_t& d2d_bytes,
                                    double& d2h_seconds, double& h2d_seconds,
                                    double& d2d_seconds) noexcept {
        const double seconds = static_cast<double>(observation.elapsed_ns) * 1.0e-9;
        switch (observation.direction) {
        case ContextTransferDirection::DeviceToHost:
            d2h_pages += observation.page_count;
            d2h_bytes += observation.units;
            d2h_seconds += seconds;
            break;
        case ContextTransferDirection::HostToDevice:
            h2d_pages += observation.page_count;
            h2d_bytes += observation.units;
            h2d_seconds += seconds;
            break;
        case ContextTransferDirection::DeviceToDevice:
            d2d_pages += observation.page_count;
            d2d_bytes += observation.units;
            d2d_seconds += seconds;
            break;
        }
    }

    template <class Result>
    void observe_transfers(const Result& result) noexcept {
        for (const ContextTransferObservation& observation : result.transfer_observations) {
            observe_transfer(observation);
        }
    }

    template <class Result>
    void observe_operations(const Result& result) noexcept {
        context_stats_.state_moves += result.operations.state_moves;
        context_stats_.state_forks += result.operations.state_forks;
        context_stats_.state_restores += result.operations.state_restores;
        context_stats_.pressure_spill_pages += result.operations.pressure_spill_pages;
        context_stats_.pressure_private_owners_demoted_kv +=
            result.operations.pressure_private_owners_demoted_kv;
        context_stats_.pressure_private_owners_demoted_kv_only +=
            result.operations.pressure_private_owners_demoted_kv_only;
        context_stats_.partial_tail_cow_pages += result.operations.partial_tail_cow_pages;
        context_stats_.historical_fork_hits += result.operations.historical_fork_hits;
    }

    std::uint32_t lane_count_           = 0;
    std::uint32_t catalog_count_        = 0;
    std::uint32_t shared_catalog_count_ = 0;
    bool cache_enabled_                 = true;
    std::array<LogicalLaneState, kMaximumConcurrency> lanes_{};
    std::vector<CatalogEntry> catalog_;
    std::vector<SharedCatalogEntry> shared_catalog_;
    std::vector<SessionIndexEntry> session_index_;
    std::vector<PrefixIndexEntry> prefix_index_;
    std::vector<CheckpointObservation> observation_scratch_;
    std::vector<PrefixDemandRecord> demand_window_;
    std::uint32_t max_long_anchors_ = 0;
    std::array<ActiveEntry, kMaximumConcurrency> active_{};
    using ContextTransaction =
        std::variant<std::monostate, MaterializationRecord, ActiveCaptureRecord>;
    ContextTransaction transaction_;
    ContextMachineCostModel cost_model_;
    Planner planner_;
    CapturePlanner capture_planner_;
    RuntimeStats context_stats_;
    ContextTransferTotals transfer_totals_;
    std::uint64_t next_continuation_id_  = 1;
    std::uint64_t next_shared_prefix_id_ = 1;
    std::uint64_t retention_epoch_       = 0;
    std::uint64_t demand_epoch_          = 0;
};

} // namespace ninfer::runtime
