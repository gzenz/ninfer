#include "models/qwen3_5/program/planning/pressure_value_ranking.h"
#include "runtime/engine/context_cache/resource_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ninfer::PrefixReusePath;
using ninfer::RuntimeStats;
using ninfer::runtime::CancellationFlagView;
using ninfer::runtime::CheckpointKind;
using ninfer::runtime::CheckpointRecoveryAlternativeWork;
using ninfer::runtime::CheckpointRef;
using ninfer::runtime::CheckpointScope;
using ninfer::runtime::CommitDisposition;
using ninfer::runtime::ConsumeStatus;
using ninfer::runtime::ContextOperationCounts;
using ninfer::runtime::ContextTransactionInProgress;
using ninfer::runtime::ContextTransactionReserveStatus;
using ninfer::runtime::ContextTransactionStatus;
using ninfer::runtime::ContextTransferObservation;
using ninfer::runtime::ContextTransferRequirement;
using ninfer::runtime::FinishDisposition;
using ninfer::runtime::LaneId;
using ninfer::runtime::MaterializationMachineWork;
using ninfer::runtime::PrefillWork;
using ninfer::runtime::PlanningCandidateId;
using ninfer::runtime::PlanningOwnerId;
using ninfer::runtime::PrivateSourceMode;
using ninfer::runtime::ProgramResourceRevision;
using ninfer::runtime::Readiness;
using ninfer::runtime::RequestPlanSummary;
using ninfer::runtime::RetentionClass;
using ninfer::runtime::VictimDisposition;

int failures = 0;
// A DENOMINATOR. "Exactly one failure, the recorded baseline" is not a checkable statement when the suite
// reports only failures -- a run that stopped early, or that never reached a case, reads the same. The count
// is printed at the end for the same reason every instrument in this repo prints its denominator.
int tests_run = 0;

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <class Test>
void run_test(const char* name, Test&& test) {
    ++tests_run;
    try {
        test();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

ninfer::runtime::ContextMachineCostModel test_cost_model() {
    ninfer::runtime::ContextMachineCostModel model;
    for (auto& transfer : model.transfer) {
        transfer.batch_ns        = 0;
        transfer.operation_ns    = 0;
        transfer.ns_per_byte_q32 = ninfer::runtime::kContextCostQ32One;
    }
    model.prefill.token_ns_q32          = 100ULL * ninfer::runtime::kContextCostQ32One;
    model.prefill.attention_pair_ns_q32 = ninfer::runtime::kContextCostQ32One;
    model.prefill.vision_item_ns        = 1;
    model.prefill.vision_patch_ns_q32   = ninfer::runtime::kContextCostQ32One;
    return model;
}

void set_fake_machine_costs(MaterializationMachineWork& work, std::uint64_t optimistic_ns,
                            std::uint64_t immediate_ns) {
    work.optimistic_candidate_transfers                  = {};
    work.candidate_transfers                             = {};
    work.pressure_transfers                              = {};
    work.remaining_prefill_work                          = {};
    work.optimistic_candidate_transfers[2].payload_bytes = optimistic_ns;
    work.candidate_transfers[2].payload_bytes            = immediate_ns;
}

CheckpointRecoveryAlternativeWork fake_recovery_work(std::uint64_t ns) {
    CheckpointRecoveryAlternativeWork work;
    work.prefill.attention_pairs = ns;
    return work;
}

struct FakePreparedPrompt {
    std::uint32_t content_key = 0;
    // Caps the token match BELOW the entry's ledger length, which is the only way the fake can produce a
    // Diverged match. Without it `split.tokens` was always either 0 or the full ledger, so the Diverged branch
    // below was dead code and the probe fields could never be set -- the second review pass found exactly that.
    std::uint32_t match_limit = 0;
};

struct FakeCacheSessionKey {
    std::uint32_t value = 0;

    [[nodiscard]] std::string_view view() const noexcept {
        return {reinterpret_cast<const char*>(&value), sizeof(value)};
    }

    friend bool operator==(FakeCacheSessionKey, FakeCacheSessionKey) = default;
};

struct FakeShortlistKey {
    std::uint32_t digest    = 0;
    std::uint32_t frontier  = 0;
    // Match the real PrefixShortlistKey surface used by [candgen] instrumentation.
    std::array<std::uint64_t, 2> digests{};
    std::uint32_t identity_tag               = 0;

    friend bool operator==(FakeShortlistKey, FakeShortlistKey) = default;
};

struct FakeRequiredKV {
    std::uint32_t main_pages    = 1;
    std::uint32_t backend_pages = 0;

    friend bool operator==(FakeRequiredKV, FakeRequiredKV) = default;
};

struct FakeCheckpointSummary {
    CheckpointRef ref;
    CheckpointScope scope = CheckpointScope::Private;
    FakeShortlistKey shortlist_key;
    ninfer::runtime::ReplicaResidency state_residency =
        ninfer::runtime::ReplicaResidency::DeviceOnly;
    FakeRequiredKV required_kv;
    PrefillWork rebuild_work;

    friend bool operator==(const FakeCheckpointSummary&, const FakeCheckpointSummary&) = default;
};

struct FakeContinuationSummary {
    std::optional<FakeCheckpointSummary> endpoint;
    std::optional<FakeCheckpointSummary> rewrite;
    std::vector<FakeCheckpointSummary> long_anchors;
    std::uint32_t active_references = 0;
};

struct FakeSharedPrefixSummary {
    FakeCheckpointSummary checkpoint;
    std::uint32_t active_references = 0;

    friend bool operator==(const FakeSharedPrefixSummary&,
                           const FakeSharedPrefixSummary&) = default;
};

FakeCheckpointSummary endpoint(std::uint32_t digest, std::uint32_t frontier) {
    return FakeCheckpointSummary{
        .ref           = CheckpointRef{.kind     = CheckpointKind::SessionEndpoint,
                                       .frontier = frontier,
                                       .ordinal  = 0},
        .scope         = CheckpointScope::Private,
        .shortlist_key = FakeShortlistKey{.digest = digest, .frontier = frontier},
        .required_kv   = FakeRequiredKV{.main_pages = 1, .backend_pages = 0},
        .rebuild_work  = PrefillWork{.tokens = frontier},
    };
}

FakeCheckpointSummary rewrite_checkpoint(std::uint32_t digest, std::uint32_t frontier) {
    return FakeCheckpointSummary{
        .ref =
            CheckpointRef{.kind = CheckpointKind::TurnClosure, .frontier = frontier, .ordinal = 0},
        .scope         = CheckpointScope::Private,
        .shortlist_key = FakeShortlistKey{.digest = digest, .frontier = frontier},
        .required_kv   = FakeRequiredKV{.main_pages = 1, .backend_pages = 0},
        .rebuild_work  = PrefillWork{.tokens = frontier},
    };
}

FakeCheckpointSummary long_anchor(std::uint32_t digest, std::uint32_t frontier,
                                  std::uint32_t ordinal) {
    return FakeCheckpointSummary{
        .ref           = CheckpointRef{.kind     = CheckpointKind::LongAnchor,
                                       .frontier = frontier,
                                       .ordinal  = ordinal},
        .scope         = CheckpointScope::Private,
        .shortlist_key = FakeShortlistKey{.digest = digest, .frontier = frontier},
        .required_kv   = FakeRequiredKV{.main_pages = 1, .backend_pages = 0},
        .rebuild_work  = PrefillWork{.tokens = frontier},
    };
}

FakeCheckpointSummary shared_checkpoint(std::uint32_t digest, std::uint32_t frontier) {
    return FakeCheckpointSummary{
        .ref           = CheckpointRef{.kind     = CheckpointKind::SharedStablePrefix,
                                       .frontier = frontier,
                                       .ordinal  = 0},
        .scope         = CheckpointScope::Shared,
        .shortlist_key = FakeShortlistKey{.digest = digest, .frontier = frontier},
        .required_kv   = FakeRequiredKV{.main_pages = 1, .backend_pages = 0},
        .rebuild_work  = PrefillWork{.tokens = frontier},
    };
}

struct FakeContextCache {
    struct Opportunity {
        ninfer::PromptCacheMarkerKind kind = ninfer::PromptCacheMarkerKind::SharedStablePrefix;
        ninfer::SharedCandidateEvidence evidence =
            ninfer::SharedCandidateEvidence::ExplicitBoundary;
        std::uint32_t frontier = 0;
    };

    std::optional<FakeCacheSessionKey> session_key;
    RetentionClass retention  = RetentionClass::RecentPrivate;
    bool update_session_index = true;
    std::vector<Opportunity> opportunities;
};

struct FakeRequestBasePlan {
    RequestPlanSummary value;
    FakeContextCache cache;
    std::uint32_t shortlist_digest = 0;
    // Refuses the shortlist key AT ONE FRONTIER, so a slot's entries can differ in outcome. Without it the
    // fake's key is refusal-free or refused-everywhere, every entry of a slot takes the same outcome, and the
    // endpoint-vs-anchor discriminator is never exercised -- the fourth review pass built a mutant replacing
    // that comparison with `true` and it survived.
    std::uint32_t refuse_frontier = 0;
    // The identity-tag half of the key. The stored entries carry tag 0, so a non-zero value here is a genuine
    // mismatch -- and because the tag check runs BEFORE the digest comparison, it is the only way to reach
    // reason 4. Without it that site had no test and deleting it survived.
    std::uint32_t identity_tag = 0;
    bool allow_shortlist           = true;
    bool isolated_feasible         = true;

    [[nodiscard]] const RequestPlanSummary& summary() const noexcept { return value; }

    [[nodiscard]] const FakeContextCache& context_cache() const noexcept { return cache; }

    [[nodiscard]] std::optional<FakeShortlistKey>
    prefix_shortlist_key(std::uint32_t frontier) const noexcept {
        if (!allow_shortlist || frontier == 0) { return std::nullopt; }
        return FakeShortlistKey{.digest       = frontier == refuse_frontier ? 99U : shortlist_digest,
                                .frontier     = frontier,
                                .identity_tag = identity_tag};
    }

    [[nodiscard]] std::size_t prefix_shortlist_size() const noexcept {
        return allow_shortlist ? 1 : 0;
    }

    [[nodiscard]] std::optional<PrefillWork>
    shared_candidate_rebuild_work(std::uint32_t frontier) const noexcept {
        const auto found =
            std::find_if(cache.opportunities.begin(), cache.opportunities.end(),
                         [&](const auto& opportunity) { return opportunity.frontier == frontier; });
        return found == cache.opportunities.end()
                   ? std::nullopt
                   : std::optional<PrefillWork>(PrefillWork{.tokens = frontier});
    }
};

FakeRequestBasePlan make_base(std::uint32_t digest,
                              std::optional<FakeCacheSessionKey> session = std::nullopt,
                              RetentionClass retention  = RetentionClass::RecentPrivate,
                              bool update_session_index = true) {
    FakeRequestBasePlan out;
    out.value.prompt_tokens           = 64;
    out.value.requested_output_tokens = 8;
    out.value.effective_output_tokens = 8;
    out.value.service_work_quanta     = 64;
    out.value.publish_continuation    = true;
    out.cache.session_key             = session;
    out.cache.retention               = retention;
    out.cache.update_session_index    = update_session_index;
    out.shortlist_digest              = digest;
    return out;
}

struct FakeContinuationHandle {
    std::uint32_t id          = 0;
    std::uint32_t content_key = 0;

    FakeContinuationHandle() = default;

    FakeContinuationHandle(std::uint32_t id_value, std::uint32_t key_value)
        : id(id_value), content_key(key_value) {}

    FakeContinuationHandle(FakeContinuationHandle&& other) noexcept
        : id(std::exchange(other.id, 0)), content_key(other.content_key) {}

    FakeContinuationHandle& operator=(FakeContinuationHandle&& other) noexcept {
        id          = std::exchange(other.id, 0);
        content_key = other.content_key;
        return *this;
    }

    FakeContinuationHandle(const FakeContinuationHandle&)            = delete;
    FakeContinuationHandle& operator=(const FakeContinuationHandle&) = delete;
};

struct FakeSharedPrefixHandle {
    std::uint32_t id          = 0;
    std::uint32_t content_key = 0;

    FakeSharedPrefixHandle()                                             = default;
    FakeSharedPrefixHandle(FakeSharedPrefixHandle&&) noexcept            = default;
    FakeSharedPrefixHandle& operator=(FakeSharedPrefixHandle&&) noexcept = default;
    FakeSharedPrefixHandle(const FakeSharedPrefixHandle&)                = delete;
    FakeSharedPrefixHandle& operator=(const FakeSharedPrefixHandle&)     = delete;
};

struct FakeSequenceHandle {
    std::uint32_t id = 0;

    friend bool operator==(FakeSequenceHandle, FakeSequenceHandle) = default;
};

struct FakeCaptureOffer {
    std::uint32_t id = 0;
};

struct FakeAdmissionCandidate {
    RequestPlanSummary value;
    PrefillWork remaining;
    std::vector<ContextTransferRequirement> transfers;
    ninfer::runtime::IdentityMaterializationAssessment identity;
    PrivateSourceMode source_mode           = PrivateSourceMode::ConsumeToActive;
    std::uint32_t private_source_id         = 0;
    std::uint32_t shared_source_id          = 0;
    std::uint32_t shared_source_content_key = 0;
    std::uint32_t shared_source_frontier    = 0;

    [[nodiscard]] const RequestPlanSummary& summary() const noexcept { return value; }

    [[nodiscard]] const ninfer::runtime::IdentityMaterializationAssessment&
    identity_assessment() const noexcept {
        return identity;
    }
};

struct FakeTargetDecision {
    std::uint64_t id                  = 0;
    std::uint64_t immediate_ns        = 100'000'000;
    std::uint32_t degradation_units   = 1;
    std::uint32_t dropped_checkpoints = 0;
    bool evicts_continuation          = false;
    bool shared_owner                 = false;

    friend bool operator==(const FakeTargetDecision&, const FakeTargetDecision&) = default;
};

struct FakePressureTargetHandle {
    std::uint32_t generation = 0;
    std::uint32_t index      = 0;

    friend bool operator==(FakePressureTargetHandle, FakePressureTargetHandle) = default;
};

struct FakePreparedPressureExpansion {
    std::uint32_t generation         = 0;
    std::uint32_t scratch_generation = 0;
    std::uint32_t new_count          = 0;

    FakePreparedPressureExpansion(std::uint32_t generation_value,
                                  std::uint32_t scratch_generation_value,
                                  std::uint32_t new_count_value) noexcept
        : generation(generation_value), scratch_generation(scratch_generation_value),
          new_count(new_count_value) {}

    FakePreparedPressureExpansion(FakePreparedPressureExpansion&&) noexcept            = default;
    FakePreparedPressureExpansion& operator=(FakePreparedPressureExpansion&&) noexcept = default;
    FakePreparedPressureExpansion(const FakePreparedPressureExpansion&)                = delete;
    FakePreparedPressureExpansion& operator=(const FakePreparedPressureExpansion&)     = delete;

    [[nodiscard]] std::uint32_t new_canonical_count() const noexcept { return new_count; }
};

struct FakePressureExpansionView {
    std::span<const FakePressureTargetHandle> children;
    std::uint32_t new_canonical_count = 0;
    bool complete                     = true;
};

class FakeAssessedPressureTarget {
public:
    FakeAssessedPressureTarget(FakeAssessedPressureTarget&&) noexcept            = default;
    FakeAssessedPressureTarget& operator=(FakeAssessedPressureTarget&&) noexcept = default;
    FakeAssessedPressureTarget(const FakeAssessedPressureTarget&)                = delete;
    FakeAssessedPressureTarget& operator=(const FakeAssessedPressureTarget&)     = delete;

    [[nodiscard]] const ninfer::runtime::PressureTargetAssessment& assessment() const noexcept {
        return assessment_;
    }

private:
    FakeAssessedPressureTarget(FakePressureTargetHandle target,
                               ninfer::runtime::PressureTargetAssessment assessment) noexcept
        : target_(target), assessment_(assessment) {}

    FakePressureTargetHandle target_;
    ninfer::runtime::PressureTargetAssessment assessment_;

    friend class FakePressurePlanningSession;
};

struct FakeResourcePlan {
    FakeAdmissionCandidate admission;
    ProgramResourceRevision revision;
    std::vector<FakeTargetDecision> private_actions;
    std::vector<FakeTargetDecision> shared_actions;
    std::vector<std::uint32_t> private_owner_ids;
    std::vector<std::uint32_t> shared_owner_ids;
    std::vector<PlanningOwnerId> private_planning_ids;
    std::vector<PlanningOwnerId> shared_planning_ids;

    FakeResourcePlan() = default;

    FakeResourcePlan(FakeAdmissionCandidate admission_value, ProgramResourceRevision revision_value)
        : admission(std::move(admission_value)), revision(revision_value) {}

    FakeResourcePlan(FakeResourcePlan&&) noexcept            = default;
    FakeResourcePlan& operator=(FakeResourcePlan&&) noexcept = default;
    FakeResourcePlan(const FakeResourcePlan&)                = delete;
    FakeResourcePlan& operator=(const FakeResourcePlan&)     = delete;

    [[nodiscard]] const RequestPlanSummary& summary() const noexcept { return admission.summary(); }

    [[nodiscard]] bool needs_transfer() const noexcept { return !admission.transfers.empty(); }

    [[nodiscard]] ProgramResourceRevision resource_revision() const noexcept { return revision; }
};

struct FakePersistentBackfillProof {
    ProgramResourceRevision revision;

    [[nodiscard]] ProgramResourceRevision resource_revision() const noexcept { return revision; }
};

struct FakeStartResult {
    FakeSequenceHandle sequence;
};

struct FakeMaterializationVictimResult {
    PlanningOwnerId owner;
    VictimDisposition disposition = VictimDisposition::Retained;
    bool pressure_committed       = false;
    std::optional<FakeContinuationSummary> final_summary;
};

struct FakeMaterializationSharedVictimResult {
    PlanningOwnerId owner;
    VictimDisposition disposition = VictimDisposition::Retained;
    bool pressure_committed       = false;
    std::optional<FakeSharedPrefixSummary> final_summary;
};

struct FakeMaterializationSourceResult {
    PrivateSourceMode mode = PrivateSourceMode::Retain;
    std::optional<FakeContinuationSummary> final_summary;
};

struct FakeMaterializationSharedSourceResult {
    std::optional<FakeSharedPrefixSummary> final_summary;
};

struct FakeMaterializationResult {
    ContextTransactionStatus status = ContextTransactionStatus::Aborted;
    std::optional<FakeStartResult> published;
    std::optional<FakeMaterializationSourceResult> source;
    std::optional<FakeMaterializationSharedSourceResult> shared_source;
    std::vector<FakeMaterializationVictimResult> victims;
    std::vector<FakeMaterializationSharedVictimResult> shared_victims;
    std::vector<ContextTransferObservation> transfer_observations;
    ContextOperationCounts operations;
};

struct FakeSharedPrefixPublication {
    FakeSharedPrefixHandle handle;
    FakeSharedPrefixSummary summary;
};

struct FakeActiveCaptureResult {
    ContextTransactionStatus status     = ContextTransactionStatus::Aborted;
    bool capacity_preparation_committed = false;
    FakeContinuationSummary active_summary;
    std::optional<FakeSharedPrefixPublication> shared;
    std::vector<FakeMaterializationVictimResult> victims;
    std::vector<FakeMaterializationSharedVictimResult> shared_victims;
    std::vector<ContextTransferObservation> transfer_observations;
    ContextOperationCounts operations;
};

using FakeContextTransactionProgress =
    std::variant<ContextTransactionInProgress, FakeMaterializationResult, FakeActiveCaptureResult>;

struct FakeCaptureAssessment {
    FakeShortlistKey shortlist_key;
    ninfer::SharedCandidateEvidence shared_evidence = ninfer::SharedCandidateEvidence::None;
    PrefillWork protected_rebuild_work;
    std::vector<ContextTransferRequirement> transfer_requirements;
    std::vector<CheckpointRecoveryAlternativeWork> projected_recovery_work{fake_recovery_work(0)};
    std::vector<CheckpointRef> private_replacement_candidates;
    bool publishes_private   = false;
    bool publishes_shared    = false;
    bool needs_transfer      = false;
    bool physically_feasible = true;
};

struct FakeTimings {
    std::uint64_t value = 0;
};

struct FakeSpeculativeStats {
    std::uint64_t value = 0;
};

struct FakeFinishResult {
    ConsumeStatus status          = ConsumeStatus::InvariantMismatch;
    FinishDisposition disposition = FinishDisposition::Released;
    FakeTimings timings;
    FakeSpeculativeStats speculative;
    FakeContinuationSummary summary;
    std::optional<FakeContinuationHandle> continuation;
};

struct FakeAbortResult {
    ConsumeStatus status = ConsumeStatus::InvariantMismatch;
    FakeTimings timings;
    FakeSpeculativeStats speculative;
};

struct FakeReleaseResult {
    ConsumeStatus status = ConsumeStatus::InvariantMismatch;
};

struct FakeCommitRowResult {
    CommitDisposition disposition = CommitDisposition::Active;
};

struct FakeCommitResult {
    std::array<FakeCommitRowResult, ninfer::kMaximumConcurrency> rows{};
    std::size_t row_count = 0;
};

struct FakeDiscardResult {
    ConsumeStatus status  = ConsumeStatus::InvariantMismatch;
    std::size_t row_count = 0;
};

struct FakePhysicalUsage {
    std::uint32_t device_state_slots      = 0;
    std::uint32_t host_state_slots        = 0;
    std::uint32_t device_main_kv_pages    = 0;
    std::uint32_t device_backend_kv_pages = 0;
    std::size_t host_kv_bytes             = 0;
};

class FakeProgram;

class FakePressurePlanningSession {
public:
    FakePressurePlanningSession(FakeProgram& program,
                                std::span<const FakeAdmissionCandidate* const> candidates,
                                std::span<const PlanningCandidateId> candidate_ids,
                                std::span<const FakeContinuationHandle* const> private_owners,
                                std::span<const PlanningOwnerId> private_owner_ids,
                                std::span<const FakeSharedPrefixHandle* const> shared_owners,
                                std::span<const PlanningOwnerId> shared_owner_ids);

    FakePressurePlanningSession(FakePressurePlanningSession&&) noexcept            = default;
    FakePressurePlanningSession& operator=(FakePressurePlanningSession&&) noexcept = default;
    FakePressurePlanningSession(const FakePressurePlanningSession&)                = delete;
    FakePressurePlanningSession& operator=(const FakePressurePlanningSession&)     = delete;

    [[nodiscard]] FakePressureTargetHandle identity_target(PlanningCandidateId candidate) const;
    [[nodiscard]] FakePressureTargetHandle identity_target() const;

    [[nodiscard]] static constexpr PlanningCandidateId candidate_id() noexcept {
        return PlanningCandidateId{.value = 0};
    }

    [[nodiscard]] FakePressureTargetHandle root_maximal_target(PlanningCandidateId candidate);
    struct Cursor;
    // `nullopt` models the target arena being full: a SEARCH alternative the planner must be able
    // to give up on. `refuse_maximal_targets` is what lets a test drive that branch -- without it the
    // graceful path is unreachable from here and a mutant that re-throws would survive.
    bool refuse_maximal_targets = false;
    [[nodiscard]] std::optional<FakePressureTargetHandle>
    maximal_target(PlanningCandidateId candidate);
    [[nodiscard]] Cursor begin_construction(FakePressureTargetHandle target, bool restore = false);
    [[nodiscard]] ninfer::runtime::PressureConstructionStep
    next_construction_option(Cursor& cursor);
    void choose_construction(Cursor& cursor, ninfer::runtime::PressureConstructionOptionId option);
    [[nodiscard]] std::optional<FakePressureTargetHandle> construction_target(const Cursor& cursor);
    [[nodiscard]] ninfer::runtime::PressureTargetGuidance guidance(FakePressureTargetHandle target);
    [[nodiscard]] FakeAssessedPressureTarget assess(FakePressureTargetHandle target);
    [[nodiscard]] FakePreparedPressureExpansion
    prepare_expansion(FakePressureTargetHandle parent,
                      std::uint32_t maximum_owners = std::numeric_limits<std::uint32_t>::max());
    [[nodiscard]] FakePressureExpansionView
    commit_expansion(FakePreparedPressureExpansion&& prepared);
    void discard_expansion(FakePreparedPressureExpansion&& prepared) noexcept;
    [[nodiscard]] PrefillWork
    shared_capture_split_prefill_work(const FakeAssessedPressureTarget&, const FakePreparedPrompt&,
                                      std::span<const std::uint32_t> frontiers) const;
    [[nodiscard]] std::optional<FakeResourcePlan> seal(FakeAssessedPressureTarget&& assessed,
                                                       const FakePreparedPrompt& prompt,
                                                       ninfer::runtime::FinalScheduleIntent intent);
    [[nodiscard]] std::optional<FakeResourcePlan> seal(FakeAssessedPressureTarget&& assessed);
    [[nodiscard]] std::optional<FakeResourcePlan>
    seal_capture(FakeAssessedPressureTarget&& assessed);
    [[nodiscard]] bool try_claim_seal_window() noexcept;
    void release_seal_window() noexcept;

private:
    struct Owner {
        const FakeContinuationHandle* private_handle = nullptr;
        const FakeSharedPrefixHandle* shared_handle  = nullptr;
        PlanningOwnerId id;
        bool shared = false;
    };

    struct Target {
        std::uint32_t candidate_index = 0;
        std::vector<std::uint16_t> choices;
        std::uint32_t stable_ordinal       = 0;
        bool root_maximal                  = false;
        std::uint32_t next_expansion_owner = 0;
    };

public:
    struct Cursor {
        Target target;
        std::vector<Target> options;
        std::size_t next_owner = 0, next_option = 0;
        std::uint32_t generation = 0, scan = 1;
        bool restore = false;
    };
private:
    std::uint32_t construction_generation_ = 0;
    [[nodiscard]] ninfer::runtime::PressureTargetGuidance guidance_for(const Target& target);
    [[nodiscard]] bool valid(FakePressureTargetHandle target) const noexcept;
    [[nodiscard]] std::uint32_t candidate_index(PlanningCandidateId candidate) const;
    void populate_options(std::uint32_t candidate_index);
    [[nodiscard]] bool same_target(const Target& left, const Target& right) const noexcept;
    [[nodiscard]] std::vector<FakeTargetDecision> decisions_for(std::uint32_t candidate_index,
                                                                std::size_t owner_index) const;

    FakeProgram* program_ = nullptr;
    ProgramResourceRevision revision_;
    std::uint32_t generation_         = 0;
    std::uint32_t scratch_generation_ = 0;
    bool scratch_live_                = false;
    std::uint32_t prepared_parent_ = 0, prepared_owner_end_ = 0;
    std::vector<const FakeAdmissionCandidate*> candidates_;
    std::vector<PlanningCandidateId> candidate_ids_;
    std::vector<Owner> owners_;
    std::vector<std::vector<std::vector<FakeTargetDecision>>> options_;
    std::vector<std::uint8_t> options_populated_;
    std::vector<Target> targets_;
    std::vector<Target> expansion_scratch_;
    std::vector<FakePressureTargetHandle> committed_children_;
    std::vector<ninfer::runtime::PressureOwnerOutcome> guidance_outcomes_;
    std::vector<ninfer::runtime::PressureOwnerOutcome> assessment_outcomes_;
    std::vector<ninfer::runtime::PressureCheckpointRecoveryImpact> assessment_impacts_;
    std::vector<CheckpointRecoveryAlternativeWork> assessment_recovery_work_;
};

class FakeProgram {
public:
    friend class FakePressurePlanningSession;

    enum class TransactionKind : std::uint8_t {
        None,
        Materialization,
        Capture,
    };

    [[nodiscard]] bool isolated_request_feasible(const FakeRequestBasePlan& base) const noexcept {
        return base.isolated_feasible;
    }

    // #6's counters, read by the stats assembly (`resource_manager.h`). The fake returns zeroes: this suite
    // is host-only and asserts the plumbing, not the eviction policy -- but it must COMPILE, and it did not
    // from the moment those two lines were added, because only the serve binary was built at the time.
    [[nodiscard]] std::uint64_t demotable_evictions() const noexcept { return 0U; }
    [[nodiscard]] std::uint64_t demotable_eviction_checks() const noexcept { return 0U; }
    // Same class as the two above: in the real Program these are incremented inside ProgramImpl's own
    // eviction/pressure TUs, which the fake does not model -- the fake never evaluates a victim's room or
    // enumerates demote options, so zero is its truthful count, not a placeholder for one.
    [[nodiscard]] std::uint64_t evictions_with_victim_room() const noexcept { return 0U; }
    [[nodiscard]] std::uint64_t pressure_options() const noexcept { return 0U; }
    [[nodiscard]] std::uint64_t demote_options() const noexcept { return 0U; }

    // The private-catalog counters. Unlike the block above, these are MUTATED BY `ResourceManager` itself
    // (planning decides them and holds only the Program façade), so the fake implements the real contract
    // (`program_impl.h`): `note_*` pre-increments and returns the new total, `add_*` accumulates. Tests can
    // therefore read what the manager recorded.
    [[nodiscard]] std::uint64_t note_publication_cell_loss() noexcept {
        return ++publication_cell_losses_;
    }
    [[nodiscard]] std::uint64_t note_publication_cell_at_risk() noexcept {
        return ++publication_cell_at_risk_;
    }
    void add_publication_cell_at_risk(std::uint32_t at_risk, std::uint32_t goals, std::uint32_t other,
                                      std::uint32_t reuse) noexcept {
        publication_cell_at_risk_runs_ += at_risk;
        publication_cell_veto_goals_ += goals;
        publication_cell_veto_other_ += other;
        publication_cell_veto_reuse_ += reuse;
    }
    void add_publication_cell_probes(std::uint64_t count) noexcept {
        publication_cell_probes_ += count;
    }
    // The UNGATED goal-failure split. Added to the fake when the manager began calling them: without these
    // the RM suite did not COMPILE, so it never ran against the change that introduced them and "the suites
    // are green" was a claim about a different set of suites.
    void add_publication_goal_blocked(std::uint64_t cell_only, std::uint64_t other) noexcept {
        publication_goal_blocked_cell_only_ += cell_only;
        publication_goal_blocked_other_ += other;
    }
    [[nodiscard]] std::uint64_t publication_goal_blocked_cell_only() const noexcept {
        return publication_goal_blocked_cell_only_;
    }
    [[nodiscard]] std::uint64_t publication_goal_blocked_other() const noexcept {
        return publication_goal_blocked_other_;
    }
    [[nodiscard]] std::uint64_t publication_cell_losses() const noexcept {
        return publication_cell_losses_;
    }
    [[nodiscard]] std::uint64_t publication_cell_probes() const noexcept {
        return publication_cell_probes_;
    }
    [[nodiscard]] std::uint64_t publication_cell_at_risk_runs() const noexcept {
        return publication_cell_at_risk_runs_;
    }
    [[nodiscard]] std::uint64_t publication_cell_veto_goals() const noexcept {
        return publication_cell_veto_goals_;
    }
    [[nodiscard]] std::uint64_t publication_cell_veto_other() const noexcept {
        return publication_cell_veto_other_;
    }
    [[nodiscard]] std::uint64_t publication_cell_veto_reuse() const noexcept {
        return publication_cell_veto_reuse_;
    }

    // THE SPLIT (`Program::prefix_split`). Contract: `tokens` = longest token-exact common prefix of the
    // prompt with the owner's stored ledger; `restorable` = deepest restorable checkpoint frontier <= tokens;
    // `identity_ok` = the identity chain also agrees at that match (only evaluated when tokens != 0); a stale
    // or unknown owner yields the empty split.
    //
    // The fake's model of a ledger, which is INVENTED and must be read as such: the fake prompt carries only
    // a `content_key`, and the fake's own reuse rule (`inspect_admission`) is "same content_key = the prompt
    // extends that owner's history". So an owner the fake itself PUBLISHED (a finished continuation, or a
    // shared prefix it published at capture) has a ledger of its published frontier and content key; a
    // prompt with the same key matches the whole ledger, any other key matches nothing. The restorable set
    // is the owner's CURRENT checkpoint summary as the fake last reported it (finish, or a retained victim's
    // final summary after checkpoint drops). identity_ok = (tokens != 0): the fake has no render, so it
    // cannot model a same-tokens/different-render divergence. Owners the fake never published (handles a
    // test constructs by hand) are unknown to it and give the empty split, like a stale handle does.
    // The fake mirrors `Program::PrefixSplit`'s surface AND, since the second review pass, the values the
    // manager consumes (`match_end`, `stored`, `probe_index`). It fell behind the real one twice (the split's
    // `match_end`/`stored` and the divergence probe), and each time this test stopped COMPILING -- so it ran
    // nothing at all while looking like a suite that simply had no new failures. A host test that cannot
    // build is the same failure mode as an instrument that cannot fire.
    struct PrefixSplit {
        std::uint32_t tokens      = 0;
        std::uint32_t restorable  = 0;
        bool          identity_ok = false;
        std::uint8_t  match_end   = 0;
        std::uint32_t stored      = 0;
        std::uint32_t probe_index = 0;
        ninfer::runtime::DivergencePosition divergence;
    };
    [[nodiscard]] PrefixSplit prefix_split(const FakeContinuationHandle& owner,
                                           const FakePreparedPrompt& prompt) const {
        ++prefix_split_calls;
        const auto found = private_ledgers_.find(owner.id);
        if (owner.id == 0 || found == private_ledgers_.end()) { return {}; }
        const PrivateLedger& ledger = found->second;
        PrefixSplit split;
        split.tokens = ledger.content_key == prompt.content_key ? ledger.length : 0U;
        if (prompt.match_limit != 0U && split.tokens > prompt.match_limit) { split.tokens = prompt.match_limit; }
        if (split.tokens == 0U) { return split; }
        // THE VALUES, not just the shape. Adding the fields so the suite COMPILES left every manager-level
        // assertion about them vacuous: the fake returned defaults, so a mutant dropping `.stored = split.stored`
        // or `.match_end = split.match_end` in the manager survived. The fake has no prompt length, so it
        // models StoredEnded (the match consumed the entry's whole ledger) and Diverged, and does NOT model
        // PromptEnded; `probe_index` follows the real rule (set whenever diverged, gate or no gate).
        split.stored     = ledger.length;
        split.match_end  = split.tokens >= ledger.length ? 1U /*StoredEnded*/ : 0U /*Diverged*/;
        if (split.match_end == 0U) {
            split.probe_index = split.tokens;
            // THE SAME LESSON AS `stored`/`match_end` ABOVE, for the divergence attribution: a fake that
            // leaves it default makes every manager-level assertion about it vacuous, so a mutant dropping
            // `.split_message_index = best.divergence.message_index` in the manager survives. The fake has no
            // prompt, so it supplies SYNTHETIC frontiers and calls the REAL mapping -- the engine's own rule
            // is then what the manager test pins, not a number invented here.
            const std::array<std::optional<std::uint32_t>, 3> frontiers{0U, 5U, 16U};
            const std::array<ninfer::ChatRole, 2> roles{ninfer::ChatRole::System,
                                                        ninfer::ChatRole::User};
            split.divergence = ninfer::runtime::attribute_divergence(frontiers, roles, split.tokens);
        }
        split.identity_ok = true;
        const auto consider = [&](const std::optional<FakeCheckpointSummary>& checkpoint) {
            if (checkpoint && checkpoint->ref.frontier <= split.tokens) {
                split.restorable = std::max(split.restorable, checkpoint->ref.frontier);
            }
        };
        consider(ledger.summary.endpoint);
        consider(ledger.summary.rewrite);
        for (const FakeCheckpointSummary& anchor : ledger.summary.long_anchors) { consider(anchor); }
        return split;
    }
    [[nodiscard]] PrefixSplit prefix_split(const FakeSharedPrefixHandle& owner,
                                           const FakePreparedPrompt& prompt) const {
        ++prefix_split_calls;
        const auto found = shared_ledgers_.find(owner.id);
        if (owner.id == 0 || found == shared_ledgers_.end()) { return {}; }
        const SharedLedger& ledger = found->second;
        PrefixSplit split;
        split.tokens = ledger.content_key == prompt.content_key ? ledger.length : 0U;
        if (prompt.match_limit != 0U && split.tokens > prompt.match_limit) { split.tokens = prompt.match_limit; }
        if (split.tokens == 0U) { return split; }
        // Modelled the same way as the private overload; it returned defaults for these fields before.
        split.stored    = ledger.length;
        split.match_end = split.tokens >= ledger.length ? 1U /*StoredEnded*/ : 0U /*Diverged*/;
        if (split.match_end == 0U) {
            split.probe_index = split.tokens;
            // THE SAME LESSON AS `stored`/`match_end` ABOVE, for the divergence attribution: a fake that
            // leaves it default makes every manager-level assertion about it vacuous, so a mutant dropping
            // `.split_message_index = best.divergence.message_index` in the manager survives. The fake has no
            // prompt, so it supplies SYNTHETIC frontiers and calls the REAL mapping -- the engine's own rule
            // is then what the manager test pins, not a number invented here.
            const std::array<std::optional<std::uint32_t>, 3> frontiers{0U, 5U, 16U};
            const std::array<ninfer::ChatRole, 2> roles{ninfer::ChatRole::System,
                                                        ninfer::ChatRole::User};
            split.divergence = ninfer::runtime::attribute_divergence(frontiers, roles, split.tokens);
        }
        split.identity_ok = true;
        if (ledger.checkpoint_frontier <= split.tokens) { split.restorable = ledger.checkpoint_frontier; }
        return split;
    }

    [[nodiscard]] std::optional<FakeAdmissionCandidate>
    inspect_admission(const FakePreparedPrompt& prompt, const FakeRequestBasePlan& base, LaneId,
                      const FakeContinuationHandle* source,
                      const FakeSharedPrefixHandle* shared_source,
                      std::optional<CheckpointRef> checkpoint, bool must_retain_source) {
        ++admission_inspections;
        if (source != nullptr) {
            inspected_private_sources.push_back(source->id);
            if (source->content_key != prompt.content_key || !checkpoint) { return std::nullopt; }
        }
        if (shared_source != nullptr) {
            inspected_shared_sources.push_back(shared_source->id);
            if (shared_source->content_key != prompt.content_key || !checkpoint) {
                return std::nullopt;
            }
        }

        FakeAdmissionCandidate plan;
        plan.value = base.summary();
        if (checkpoint) {
            plan.value.reusable_prompt_tokens = checkpoint->frontier;
            switch (checkpoint->kind) {
            case CheckpointKind::SessionEndpoint:
                plan.value.prefix_reuse_path = PrefixReusePath::PrivateEndpoint;
                break;
            case CheckpointKind::TurnClosure:
                plan.value.prefix_reuse_path = PrefixReusePath::PrivateTurnClosure;
                break;
            case CheckpointKind::ResponseReplay:
                plan.value.prefix_reuse_path = PrefixReusePath::PrivateResponseReplay;
                break;
            case CheckpointKind::LongAnchor:
                plan.value.prefix_reuse_path = PrefixReusePath::PrivateLongAnchor;
                break;
            case CheckpointKind::SharedStablePrefix:
                plan.value.prefix_reuse_path = PrefixReusePath::SharedStablePrefix;
                break;
            }
        } else {
            plan.value.reusable_prompt_tokens = 0;
            plan.value.prefix_reuse_path      = PrefixReusePath::Root;
        }
        plan.remaining.tokens = plan.value.prompt_tokens > plan.value.reusable_prompt_tokens
                                    ? plan.value.prompt_tokens - plan.value.reusable_prompt_tokens
                                    : 0;
        if (source != nullptr) {
            plan.private_source_id = source->id;
            plan.source_mode =
                must_retain_source ? PrivateSourceMode::Retain : PrivateSourceMode::ConsumeToActive;
        } else if (shared_source != nullptr) {
            plan.shared_source_id          = shared_source->id;
            plan.shared_source_content_key = shared_source->content_key;
            plan.shared_source_frontier    = checkpoint->frontier;
            plan.source_mode               = PrivateSourceMode::Retain;
        }
        plan.identity.machine_work.remaining_prefill_work = plan.remaining;
        plan.identity.machine_work.reused_prompt_tokens   = plan.value.reusable_prompt_tokens;
        plan.identity.physical_status =
            target_feasible(std::span<const FakeTargetDecision>{})
                ? ninfer::runtime::MaterializationPhysicalStatus::Feasible
                : ninfer::runtime::MaterializationPhysicalStatus::Infeasible;
        plan.identity.source_mode = plan.source_mode;
        plan.identity.expandable  = plan.identity.physical_status !=
                                   ninfer::runtime::MaterializationPhysicalStatus::Feasible;
        plan.identity.projection_work = 1;
        plan.identity.assessment_digest =
            (static_cast<std::uint64_t>(plan.value.reusable_prompt_tokens) << 32U) ^
            revision_.value;
        return plan;
    }

    [[nodiscard]] std::optional<FakeResourcePlan>
    seal_identity(const FakeAdmissionCandidate& admission, const FakePreparedPrompt&,
                  ninfer::runtime::FinalScheduleIntent intent) {
        if (admission.identity.physical_status !=
            ninfer::runtime::MaterializationPhysicalStatus::Feasible) {
            return std::nullopt;
        }
        selected_shared_capture_frontiers.assign(intent.shared_capture_frontiers.begin(),
                                                 intent.shared_capture_frontiers.end());
        seal_attempts.emplace_back();
        return FakeResourcePlan(admission, revision_);
    }

    [[nodiscard]] FakePressurePlanningSession
    begin_pressure_planning(std::span<const FakeAdmissionCandidate* const> candidates,
                            std::span<const PlanningCandidateId> candidate_ids,
                            std::span<const FakeContinuationHandle* const> private_owners,
                            std::span<const PlanningOwnerId> private_owner_ids,
                            std::span<const FakeSharedPrefixHandle* const> shared_owners,
                            std::span<const PlanningOwnerId> shared_owner_ids);

    [[nodiscard]] PrefillWork
    shared_capture_split_prefill_work(const FakeAdmissionCandidate& candidate,
                                      const FakePreparedPrompt&,
                                      std::span<const std::uint32_t> frontiers) const noexcept {
        PrefillWork work = candidate.identity.machine_work.remaining_prefill_work;
        work.attention_pairs += static_cast<std::uint64_t>(frontiers.size()) * 100U;
        return work;
    }

    [[nodiscard]] bool
    target_feasible(std::span<const FakeTargetDecision> decisions) const noexcept {
        if (pressure_units(decisions) < required_pressure_actions) { return false; }
        const bool has_eviction =
            std::any_of(decisions.begin(), decisions.end(),
                        [](const auto& decision) { return decision.evicts_continuation; });
        if (required_action_id && !has_eviction &&
            std::none_of(decisions.begin(), decisions.end(), [&](const auto& decision) {
                return decision.id == *required_action_id;
            })) {
            return false;
        }
        if (require_evictions &&
            std::any_of(decisions.begin(), decisions.end(),
                        [](const auto& decision) { return !decision.evicts_continuation; })) {
            return false;
        }
        return true;
    }

    [[nodiscard]] std::size_t
    pressure_units(std::span<const FakeTargetDecision> decisions) const noexcept {
        std::size_t units = 0;
        for (const FakeTargetDecision& decision : decisions) {
            units += decision.evicts_continuation ? eviction_pressure_action_units : 1U;
        }
        return units;
    }

    [[nodiscard]] ContextTransactionReserveStatus
    start_resource_transaction(FakeResourcePlan&& plan, FakePreparedPrompt&& prompt,
                               CancellationFlagView cancellation) {
        ++start_calls;
        started_source_id   = plan.admission.private_source_id;
        started_source_mode = plan.admission.source_mode;
        started_action_ids.clear();
        for (const auto& action : plan.private_actions) { started_action_ids.push_back(action.id); }
        for (const auto& action : plan.shared_actions) { started_action_ids.push_back(action.id); }
        if (cancellation.requested() || abort_start || plan.revision != revision_) {
            return ContextTransactionReserveStatus::Aborted;
        }
        pending_prompt_ = prompt;
        pending_plan_.emplace(std::move(plan));
        transaction_kind_ = TransactionKind::Materialization;
        advance_revision();
        return ContextTransactionReserveStatus::Reserved;
    }

    [[nodiscard]] std::optional<FakePersistentBackfillProof>
    prove_persistent_backfill(const FakeRequestBasePlan&, const FakeResourcePlan& candidate,
                              std::span<const FakeSequenceHandle>) const {
        if (candidate.resource_revision() != revision_) { return std::nullopt; }
        return FakePersistentBackfillProof{.revision = revision_};
    }

    [[nodiscard]] FakeContextTransactionProgress
    progress_context_transaction(CancellationFlagView cancellation) {
        require(transaction_kind_ != TransactionKind::None,
                "fake Program has no context transaction");
        if (progress_in_progress_once) {
            progress_in_progress_once = false;
            return ContextTransactionInProgress{};
        }
        if (transaction_kind_ == TransactionKind::Capture) {
            FakeActiveCaptureResult result;
            result.status =
                cancellation.requested() ? ContextTransactionStatus::Aborted : capture_status;
            if (pending_plan_) {
                for (std::size_t index = 0; index < pending_plan_->private_actions.size();
                     ++index) {
                    const FakeTargetDecision& action = pending_plan_->private_actions[index];
                    FakeMaterializationVictimResult victim{
                        .owner       = pending_plan_->private_planning_ids[index],
                        .disposition = action.evicts_continuation ? VictimDisposition::Evicted
                                                                  : VictimDisposition::Retained,
                        .pressure_committed = action.evicts_continuation ||
                                              result.status == ContextTransactionStatus::Published,
                    };
                    if (!action.evicts_continuation) {
                        const std::uint32_t owner = pending_plan_->private_owner_ids[index];
                        victim.final_summary.emplace();
                        const std::uint32_t content = sequence_content_keys_.at(owner);
                        if (action.dropped_checkpoints == 0 ||
                            malform_private_checkpoint_identity) {
                            victim.final_summary->endpoint = endpoint(content, finish_frontier);
                        }
                        if (finish_with_rewrite && (action.dropped_checkpoints == 0 ||
                                                    !malform_private_checkpoint_identity)) {
                            victim.final_summary->rewrite =
                                rewrite_checkpoint(content, finish_frontier - 1U);
                        }
                        note_private_summary(owner, *victim.final_summary);
                    } else {
                        private_ledgers_.erase(pending_plan_->private_owner_ids[index]);
                    }
                    result.victims.push_back(std::move(victim));
                }
                for (std::size_t index = 0; index < pending_plan_->shared_actions.size(); ++index) {
                    const FakeTargetDecision& action = pending_plan_->shared_actions[index];
                    FakeMaterializationSharedVictimResult victim{
                        .owner       = pending_plan_->shared_planning_ids[index],
                        .disposition = action.evicts_continuation ? VictimDisposition::Evicted
                                                                  : VictimDisposition::Retained,
                        .pressure_committed = action.evicts_continuation ||
                                              result.status == ContextTransactionStatus::Published,
                    };
                    if (!action.evicts_continuation) {
                        const std::uint32_t owner = pending_plan_->shared_owner_ids[index];
                        victim.final_summary      = FakeSharedPrefixSummary{
                                 .checkpoint = shared_checkpoint(owner, finish_frontier),
                        };
                        const auto ledger = shared_ledgers_.find(owner);
                        if (ledger != shared_ledgers_.end()) {
                            ledger->second.checkpoint_frontier = finish_frontier;
                        }
                    } else {
                        shared_ledgers_.erase(pending_plan_->shared_owner_ids[index]);
                    }
                    result.shared_victims.push_back(std::move(victim));
                }
            }
            if (malform_last_capture_private_victim && !result.victims.empty()) {
                FakeMaterializationVictimResult& victim = result.victims.back();
                victim.disposition                      = VictimDisposition::Evicted;
                victim.pressure_committed               = false;
                victim.final_summary.reset();
            }
            if (reverse_pressure_results) {
                std::reverse(result.victims.begin(), result.victims.end());
                std::reverse(result.shared_victims.begin(), result.shared_victims.end());
            }
            if (result.status == ContextTransactionStatus::Published) {
                result.active_summary = capture_summary;
                if (pending_capture_publish_shared_) {
                    FakeSharedPrefixHandle handle;
                    handle.id          = next_shared_id_++;
                    handle.content_key = capture_assessment.shortlist_key.digest;
                    shared_ledgers_[handle.id] = SharedLedger{
                        .content_key         = capture_assessment.shortlist_key.digest,
                        .length              = capture_assessment.shortlist_key.frontier,
                        .checkpoint_frontier = capture_assessment.shortlist_key.frontier};
                    result.shared      = FakeSharedPrefixPublication{
                             .handle = std::move(handle),
                             .summary =
                            FakeSharedPrefixSummary{
                                     .checkpoint =
                                    shared_checkpoint(capture_assessment.shortlist_key.digest,
                                                           capture_assessment.shortlist_key.frontier),
                                     .active_references = 1,
                            },
                    };
                }
            }
            return result;
        }

        require(pending_plan_.has_value(), "fake materialization plan disappeared");
        const FakeResourcePlan& plan = *pending_plan_;
        FakeMaterializationResult result;
        result.status = (abort_progress || cancellation.requested())
                            ? ContextTransactionStatus::Aborted
                            : ContextTransactionStatus::Published;
        for (std::size_t index = 0; index < plan.private_actions.size(); ++index) {
            const FakeTargetDecision& action = plan.private_actions[index];
            const bool evicted               = action.evicts_continuation;
            FakeMaterializationVictimResult victim{
                .owner       = plan.private_planning_ids[index],
                .disposition = evicted ? VictimDisposition::Evicted : VictimDisposition::Retained,
                .pressure_committed =
                    evicted || result.status == ContextTransactionStatus::Published,
            };
            if (!evicted) {
                const std::uint32_t owner_id = plan.private_owner_ids.at(index);
                victim.final_summary.emplace();
                const std::uint32_t content = sequence_content_keys_.at(owner_id);
                if (action.dropped_checkpoints == 0 || malform_private_checkpoint_identity) {
                    victim.final_summary->endpoint = endpoint(content, finish_frontier);
                }
                if (finish_with_rewrite &&
                    (action.dropped_checkpoints == 0 || !malform_private_checkpoint_identity)) {
                    victim.final_summary->rewrite =
                        rewrite_checkpoint(content, finish_frontier - 1U);
                }
                note_private_summary(owner_id, *victim.final_summary);
            } else {
                private_ledgers_.erase(plan.private_owner_ids.at(index));
            }
            result.victims.push_back(std::move(victim));
        }
        for (std::size_t index = 0; index < plan.shared_actions.size(); ++index) {
            const FakeTargetDecision& action = plan.shared_actions[index];
            if (action.evicts_continuation) { shared_ledgers_.erase(plan.shared_owner_ids.at(index)); }
            result.shared_victims.push_back(FakeMaterializationSharedVictimResult{
                .owner              = plan.shared_planning_ids[index],
                .disposition        = action.evicts_continuation ? VictimDisposition::Evicted
                                                                 : VictimDisposition::Retained,
                .pressure_committed = action.evicts_continuation ||
                                      result.status == ContextTransactionStatus::Published,
            });
        }
        if (malform_last_private_victim && !result.victims.empty()) {
            FakeMaterializationVictimResult& victim = result.victims.back();
            victim.disposition                      = VictimDisposition::Evicted;
            victim.pressure_committed               = false;
            victim.final_summary.reset();
        }
        if (reverse_pressure_results) {
            std::reverse(result.victims.begin(), result.victims.end());
            std::reverse(result.shared_victims.begin(), result.shared_victims.end());
        }
        if (plan.admission.private_source_id != 0) {
            result.source = FakeMaterializationSourceResult{
                .mode = result.status == ContextTransactionStatus::Aborted
                            ? PrivateSourceMode::Retain
                            : plan.admission.source_mode};
        }
        if (plan.admission.shared_source_id != 0) {
            result.shared_source = FakeMaterializationSharedSourceResult{};
            if (report_shared_source_summary) {
                const std::uint32_t references =
                    result.status == ContextTransactionStatus::Published
                        ? ++reported_shared_active_references
                        : reported_shared_active_references;
                FakeCheckpointSummary checkpoint =
                    shared_checkpoint(plan.admission.shared_source_content_key,
                                      plan.admission.shared_source_frontier);
                if (change_shared_source_residency_on_second_report && references >= 2) {
                    checkpoint.state_residency = ninfer::runtime::ReplicaResidency::Both;
                }
                result.shared_source->final_summary = FakeSharedPrefixSummary{
                    .checkpoint        = std::move(checkpoint),
                    .active_references = references,
                };
            }
        }
        if (result.status == ContextTransactionStatus::Published) {
            const std::uint32_t sequence_id        = next_sequence_id_++;
            sequence_content_keys_.at(sequence_id) = pending_prompt_.content_key;
            result.published = FakeStartResult{.sequence = FakeSequenceHandle{sequence_id}};
        }
        return result;
    }

    void finalize_context_transaction() noexcept {
        transaction_kind_ = TransactionKind::None;
        pending_plan_.reset();
        pending_capture_publish_shared_ = false;
    }

    [[nodiscard]] bool has_context_transaction() const noexcept {
        return transaction_kind_ != TransactionKind::None;
    }

    // The `site` parameter mirrors the real contract (`inspect_capture`'s call-site tag, which exists for
    // the diagnostic line only): the mock must match the signature or the whole suite stops compiling,
    // which is how this was found.
    [[nodiscard]] FakeCaptureAssessment inspect_capture(const FakeCaptureOffer&,
                                                        const FakeSharedPrefixHandle*,
                                                        const FakeSharedPrefixHandle*,
                                                        std::optional<CheckpointRef>,
                                                        bool permit_shared_publication,
                                                        const char* /*site*/) const {
        FakeCaptureAssessment assessment = capture_assessment;
        if (!permit_shared_publication) { assessment.publishes_shared = false; }
        return assessment;
    }

    [[nodiscard]] std::vector<CheckpointRecoveryAlternativeWork>
    checkpoint_recovery_work(const FakeContinuationHandle&, CheckpointRef) const {
        return {fake_recovery_work(0)};
    }

    [[nodiscard]] std::vector<CheckpointRecoveryAlternativeWork>
    checkpoint_recovery_work(const FakeSharedPrefixHandle&, CheckpointRef) const {
        return {fake_recovery_work(0)};
    }

    [[nodiscard]] FakePressurePlanningSession
    begin_capture_pressure_planning(const FakeCaptureAssessment& assessment,
                                    std::span<const FakeContinuationHandle* const> private_owners,
                                    std::span<const PlanningOwnerId> private_owner_ids,
                                    std::span<const FakeSharedPrefixHandle* const> shared_owners,
                                    std::span<const PlanningOwnerId> shared_owner_ids) {
        capture_pressure_candidate_       = std::make_unique<FakeAdmissionCandidate>();
        FakeAdmissionCandidate& candidate = *capture_pressure_candidate_;
        candidate.value.prompt_tokens     = assessment.shortlist_key.frontier;
        for (const ContextTransferRequirement& transfer : assessment.transfer_requirements) {
            const std::size_t direction = static_cast<std::size_t>(transfer.direction);
            candidate.identity.machine_work.candidate_transfers[direction].payload_bytes +=
                transfer.work.payload_bytes;
            candidate.identity.machine_work.candidate_transfers[direction].copy_operations +=
                transfer.work.copy_operations;
        }
        candidate.identity.machine_work.optimistic_candidate_transfers =
            candidate.identity.machine_work.candidate_transfers;
        candidate.identity.physical_status =
            target_feasible(std::span<const FakeTargetDecision>{})
                ? ninfer::runtime::MaterializationPhysicalStatus::Feasible
                : ninfer::runtime::MaterializationPhysicalStatus::Infeasible;
        candidate.identity.expandable = candidate.identity.physical_status !=
                                        ninfer::runtime::MaterializationPhysicalStatus::Feasible;
        candidate.identity.projection_work = 1;
        candidate.identity.assessment_digest =
            (static_cast<std::uint64_t>(assessment.shortlist_key.frontier) << 32U) ^
            revision_.value;
        const FakeAdmissionCandidate* candidate_handle = &candidate;
        const std::array candidate_ids{FakePressurePlanningSession::candidate_id()};
        return begin_pressure_planning(
            std::span<const FakeAdmissionCandidate* const>(&candidate_handle, 1), candidate_ids,
            private_owners, private_owner_ids, shared_owners, shared_owner_ids);
    }

    [[nodiscard]] bool shared_capture_matches(const FakeCaptureOffer&,
                                              const FakeSharedPrefixHandle&) const {
        return false;
    }

    void skip_capture(FakeCaptureOffer&&) { ++skipped_captures; }

    [[nodiscard]] ContextTransactionReserveStatus
    reserve_active_capture(FakeCaptureOffer&&, const FakeSharedPrefixHandle*,
                           const FakeSharedPrefixHandle*, std::optional<CheckpointRef>, bool,
                           CancellationFlagView cancellation) {
        if (cancellation.requested() || abort_capture_start) {
            return ContextTransactionReserveStatus::Aborted;
        }
        pending_plan_.reset();
        pending_capture_publish_shared_ = false;
        transaction_kind_               = TransactionKind::Capture;
        advance_revision();
        return ContextTransactionReserveStatus::Reserved;
    }

    [[nodiscard]] ContextTransactionReserveStatus reserve_active_capture_with_pressure(
        FakeCaptureOffer&&, const FakeSharedPrefixHandle*, const FakeSharedPrefixHandle*,
        std::optional<CheckpointRef>, bool publish_shared, FakeResourcePlan&& pressure,
        CancellationFlagView cancellation) {
        if (cancellation.requested() || abort_capture_start || pressure.revision != revision_) {
            return ContextTransactionReserveStatus::Aborted;
        }
        started_action_ids.clear();
        for (const auto& action : pressure.private_actions) {
            started_action_ids.push_back(action.id);
        }
        for (const auto& action : pressure.shared_actions) {
            started_action_ids.push_back(action.id);
        }
        pending_plan_.emplace(std::move(pressure));
        pending_capture_publish_shared_ = publish_shared;
        transaction_kind_               = TransactionKind::Capture;
        advance_revision();
        return ContextTransactionReserveStatus::Reserved;
    }

    [[nodiscard]] FakeFinishResult finish(FakeSequenceHandle sequence) noexcept {
        ++finish_calls;
        if (finish_fail_next) {
            finish_fail_next = false;
            return {};
        }
        advance_revision();
        FakeFinishResult result;
        result.status = ConsumeStatus::Consumed;
        if (finish_release) {
            result.disposition = FinishDisposition::Released;
            return result;
        }
        result.disposition      = FinishDisposition::Catalogued;
        const std::uint32_t key = sequence_content_keys_[sequence.id];
        result.summary.endpoint = endpoint(key, finish_frontier);
        // THE CENSUS READS `state_residency`, and `endpoint()` builds every checkpoint DeviceOnly -- so without
        // this knob no fixture could produce a host-resident checkpoint and `host_state_checkpoints_*` had
        // nothing to count (two of its mutants survived on that).
        if (host_residency != std::nullopt) { result.summary.endpoint->state_residency = *host_residency; }
        if (finish_with_rewrite) {
            result.summary.rewrite = rewrite_checkpoint(key, finish_frontier - 1U);
        }
        result.continuation.emplace(sequence.id, key);
        private_ledgers_[sequence.id] =
            PrivateLedger{.content_key = key, .length = finish_frontier, .summary = result.summary};
        return result;
    }

    [[nodiscard]] FakeAbortResult abort(FakeSequenceHandle) noexcept {
        ++abort_calls;
        advance_revision();
        return FakeAbortResult{.status      = ConsumeStatus::Consumed,
                               .timings     = FakeTimings{.value = 7},
                               .speculative = FakeSpeculativeStats{.value = 9}};
    }

    [[nodiscard]] FakeReleaseResult
    release_continuation(FakeContinuationHandle&& continuation) noexcept {
        released_continuations.push_back(continuation.id);
        private_ledgers_.erase(continuation.id);
        advance_revision();
        return FakeReleaseResult{.status = ConsumeStatus::Consumed};
    }

    [[nodiscard]] ProgramResourceRevision resource_revision() const noexcept { return revision_; }

    [[nodiscard]] FakePhysicalUsage physical_usage() const noexcept { return usage; }
    // Added with the shared-replacement counter: `populate_runtime_stats` reads it, so the mock needs it.
    [[nodiscard]] std::uint64_t shared_replacements() const noexcept { return 0; }

    void invalidate_resources() noexcept { advance_revision(); }

    std::vector<std::pair<std::uint32_t, std::vector<FakeTargetDecision>>> owner_decisions;
    std::size_t required_pressure_actions       = 0;
    // Force the fake session's `maximal_target` to report a FULL TARGET ARENA, so the planner's
    // graceful branch -- the one that replaced the 2026-09-28 throw -- is reachable from a host test.
    bool refuse_maximal_targets = false;
    // THE RESCUE BRANCH HAS NO HOST COVERAGE, AND THIS KNOB IS HOW THAT IS KNOWN RATHER THAN ASSUMED.
    // `maximal_target_calls` is its denominator. Measured: in every host scenario tried it stays 0,
    // because the rescue runs in the REFINEMENT phase, which the fake does not reach -- the same gap
    // that makes `guided deep retention` fail (its message is "guided pressure search returned to eager
    // breadth-first assessment"). Consequence, verified by mutation: replacing the rescue branch's
    // graceful `break` with the pre-fix `throw std::length_error` SURVIVES this whole suite (51 run,
    // 1 failed -- the baseline only). The construction branch is covered instead
    // (`test_target_arena_bound_degrades_instead_of_throwing`), and that is the path that fired in
    // production, but the rescue branch stays untested until the refinement phase is reachable here.
    // Do not delete this knob: it is the measurement that keeps that residual honest.
    std::uint32_t maximal_target_calls = 0;
    // The CONSTRUCTION half of the same question. `construction_target` is reached in every guided
    // search, so this is the arm that can actually be driven -- and it is the path that fired in
    // production (`targets=4102/4102`). `construction_target_calls` is its denominator: without it a
    // scenario that never called it reports zero truncations and looks identical to a graceful stop.
    bool refuse_construction_targets = false;
    std::uint32_t construction_target_calls = 0;
    std::size_t eviction_pressure_action_units  = 1;
    std::uint32_t private_pressure_alternatives = 1;
    std::optional<std::size_t> pressure_optional_target_capacity;
    std::uint64_t pressure_action_immediate_ns      = 100'000'000;
    std::uint32_t pressure_action_degradation_units = 1;
    bool include_cumulative_private_target          = false;
    bool combined_target_cancels_pressure_copy      = false;
    std::optional<std::uint64_t> pressure_target_immediate_ns_override;
    std::optional<std::uint64_t> required_action_id;
    std::uint32_t pressure_assessment_delay_us           = 0;
    std::uint64_t pressure_checkpoint_recovery_ns        = 100;
    bool require_evictions                               = false;
    bool abort_start                                     = false;
    bool abort_progress                                  = false;
    bool malform_last_private_victim                     = false;
    bool malform_last_capture_private_victim             = false;
    bool malform_private_checkpoint_identity             = false;
    bool reverse_pressure_results                        = false;
    bool progress_in_progress_once                       = false;
    bool finish_fail_next                                = false;
    bool finish_release                                  = false;
    bool finish_with_rewrite                             = false;
    // When set, published endpoints report this residency. Only the census consumes it.
    std::optional<ninfer::runtime::ReplicaResidency> host_residency;
    bool abort_capture_start                             = false;
    bool report_shared_source_summary                    = false;
    bool change_shared_source_residency_on_second_report = false;
    std::uint32_t reported_shared_active_references      = 0;
    ContextTransactionStatus capture_status              = ContextTransactionStatus::Published;
    FakeCaptureAssessment capture_assessment;
    FakeContinuationSummary capture_summary;
    FakePhysicalUsage usage;

    std::uint64_t admission_inspections       = 0;
    std::uint64_t pressure_planning_sessions  = 0;
    std::uint64_t pressure_target_assessments = 0;
    std::uint64_t start_calls                 = 0;
    std::uint64_t finish_calls                = 0;
    std::uint64_t abort_calls                 = 0;
    std::uint64_t skipped_captures            = 0;
    std::size_t pressure_target_count_peak    = 0;
    std::uint32_t finish_frontier             = 16;
    std::uint32_t started_source_id           = 0;
    PrivateSourceMode started_source_mode     = PrivateSourceMode::ConsumeToActive;
    std::vector<std::uint32_t> inspected_private_sources;
    // The capture-skip counters the real `populate_runtime_stats` copies out. The fake skipped nothing, so
    // all five are zero -- but they have to EXIST, or the suite does not compile. NO ASSERTION READS THEM, so
    // the reason-to-field mapping in `populate_runtime_stats` (swapping two indices would survive) is
    // untested; recorded rather than implied.
    [[nodiscard]] std::uint64_t capture_skips(std::size_t) const noexcept { return 0; }
    std::vector<std::uint32_t> inspected_shared_sources;
    std::vector<std::vector<std::uint64_t>> seal_attempts;
    std::vector<std::uint64_t> started_action_ids;
    std::vector<std::uint32_t> selected_shared_capture_frontiers;
    std::vector<std::uint32_t> released_continuations;

    mutable std::uint64_t prefix_split_calls = 0;

private:
    struct PrivateLedger {
        std::uint32_t content_key = 0;
        std::uint32_t length      = 0;
        FakeContinuationSummary summary;
    };
    struct SharedLedger {
        std::uint32_t content_key         = 0;
        std::uint32_t length              = 0;
        std::uint32_t checkpoint_frontier = 0;
    };

    void advance_revision() noexcept {
        if (++revision_.value == 0) { ++revision_.value; }
    }

    // A retained private victim's checkpoint set changed (drops): the ledger is unchanged, the restorable
    // set follows the final summary the fake reported. Unknown owners stay unknown.
    void note_private_summary(std::uint32_t id, const FakeContinuationSummary& summary) {
        const auto found = private_ledgers_.find(id);
        if (found != private_ledgers_.end()) { found->second.summary = summary; }
    }

    std::uint64_t publication_cell_losses_       = 0;
    std::uint64_t publication_cell_probes_       = 0;
    std::uint64_t publication_cell_at_risk_      = 0;
    std::uint64_t publication_cell_at_risk_runs_ = 0;
    std::uint64_t publication_cell_veto_goals_   = 0;
    std::uint64_t publication_cell_veto_other_   = 0;
    std::uint64_t publication_cell_veto_reuse_   = 0;
    std::uint64_t publication_goal_blocked_cell_only_ = 0;
    std::uint64_t publication_goal_blocked_other_     = 0;
    std::map<std::uint32_t, PrivateLedger> private_ledgers_;
    std::map<std::uint32_t, SharedLedger> shared_ledgers_;

    ProgramResourceRevision revision_{.value = 1};
    std::uint32_t planning_generation_ = 0;
    std::uint32_t next_sequence_id_    = 1;
    std::uint32_t next_shared_id_      = 1;
    std::array<std::uint32_t, 256> sequence_content_keys_{};
    TransactionKind transaction_kind_ = TransactionKind::None;
    FakePreparedPrompt pending_prompt_;
    std::optional<FakeResourcePlan> pending_plan_;
    std::unique_ptr<FakeAdmissionCandidate> capture_pressure_candidate_;
    bool pending_capture_publish_shared_ = false;
};

FakePressurePlanningSession::FakePressurePlanningSession(
    FakeProgram& program, std::span<const FakeAdmissionCandidate* const> candidates,
    std::span<const PlanningCandidateId> candidate_ids,
    std::span<const FakeContinuationHandle* const> private_owners,
    std::span<const PlanningOwnerId> private_owner_ids,
    std::span<const FakeSharedPrefixHandle* const> shared_owners,
    std::span<const PlanningOwnerId> shared_owner_ids)
    : program_(&program), revision_(program.resource_revision()) {
    require(!candidates.empty() && candidates.size() == candidate_ids.size(),
            "fake pressure session has no candidate identity");
    require(private_owners.size() == private_owner_ids.size() &&
                shared_owners.size() == shared_owner_ids.size(),
            "fake pressure owner arrays are not aligned");
    candidates_.assign(candidates.begin(), candidates.end());
    candidate_ids_.assign(candidate_ids.begin(), candidate_ids.end());
    for (std::size_t index = 0; index < private_owners.size(); ++index) {
        owners_.push_back(Owner{.private_handle = private_owners[index],
                                .id             = private_owner_ids[index],
                                .shared         = false});
    }
    for (std::size_t index = 0; index < shared_owners.size(); ++index) {
        owners_.push_back(Owner{
            .shared_handle = shared_owners[index], .id = shared_owner_ids[index], .shared = true});
    }
    std::sort(owners_.begin(), owners_.end(),
              [](const Owner& left, const Owner& right) { return left.id.value < right.id.value; });
    options_.resize(candidates_.size());
    options_populated_.resize(candidates_.size());
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        require(candidates_[index] != nullptr, "fake pressure candidate is null");
        targets_.push_back(Target{
            .candidate_index = static_cast<std::uint32_t>(index),
            .choices         = std::vector<std::uint16_t>(owners_.size(), 0),
            .stable_ordinal  = static_cast<std::uint32_t>(index),
        });
    }
    program.pressure_target_count_peak =
        std::max(program.pressure_target_count_peak, targets_.size());
    if (++program.planning_generation_ == 0) { ++program.planning_generation_; }
    ++program.pressure_planning_sessions;
    generation_ = program.planning_generation_;
}

bool FakePressurePlanningSession::valid(FakePressureTargetHandle target) const noexcept {
    return program_ != nullptr && target.generation == generation_ &&
           target.index < targets_.size() && program_->resource_revision() == revision_;
}

std::uint32_t FakePressurePlanningSession::candidate_index(PlanningCandidateId candidate) const {
    const auto found = std::find(candidate_ids_.begin(), candidate_ids_.end(), candidate);
    require(found != candidate_ids_.end(), "fake pressure candidate is foreign");
    return static_cast<std::uint32_t>(found - candidate_ids_.begin());
}

bool FakePressurePlanningSession::same_target(const Target& left,
                                              const Target& right) const noexcept {
    return left.candidate_index == right.candidate_index && left.choices == right.choices;
}

std::vector<FakeTargetDecision>
FakePressurePlanningSession::decisions_for(std::uint32_t selected_candidate,
                                           std::size_t owner_index) const {
    const FakeAdmissionCandidate& candidate = *candidates_.at(selected_candidate);
    const Owner& owner                      = owners_.at(owner_index);
    if ((!owner.shared && candidate.private_source_id != 0 &&
         candidate.private_source_id == owner.private_handle->id) ||
        (owner.shared && candidate.shared_source_id != 0 &&
         candidate.shared_source_id == owner.shared_handle->id)) {
        return {};
    }

    if (!owner.shared) {
        for (const auto& [id, decisions] : program_->owner_decisions) {
            if (id == owner.private_handle->id) { return decisions; }
        }
    }
    std::vector<FakeTargetDecision> decisions;
    if (!owner.shared) {
        for (std::uint32_t index = 0; index < program_->private_pressure_alternatives; ++index) {
            decisions.push_back(FakeTargetDecision{
                .id = 1000U + owner.private_handle->id + 10000U * static_cast<std::uint64_t>(index),
                .immediate_ns      = program_->pressure_action_immediate_ns,
                .degradation_units = program_->pressure_action_degradation_units,
            });
        }
        if (program_->include_cumulative_private_target) {
            decisions.push_back(FakeTargetDecision{
                .id                  = 5000U + owner.private_handle->id,
                .degradation_units   = 2,
                .dropped_checkpoints = 1,
            });
        }
        decisions.push_back(FakeTargetDecision{
            .id                  = 2000U + owner.private_handle->id,
            .degradation_units   = 4,
            .dropped_checkpoints = 1,
            .evicts_continuation = true,
        });
    } else {
        decisions.push_back(FakeTargetDecision{
            .id           = 3000U + owner.shared_handle->id,
            .shared_owner = true,
        });
        decisions.push_back(FakeTargetDecision{
            .id                  = 4000U + owner.shared_handle->id,
            .degradation_units   = 4,
            .dropped_checkpoints = 1,
            .evicts_continuation = true,
            .shared_owner        = true,
        });
    }
    return decisions;
}

void FakePressurePlanningSession::populate_options(std::uint32_t selected_candidate) {
    require(selected_candidate < candidates_.size(), "fake pressure candidate index is invalid");
    if (options_populated_[selected_candidate] != 0) { return; }
    options_[selected_candidate].resize(owners_.size());
    for (std::size_t index = 0; index < owners_.size(); ++index) {
        options_[selected_candidate][index] = decisions_for(selected_candidate, index);
    }
    options_populated_[selected_candidate] = 1;
}

FakePressureTargetHandle
FakePressurePlanningSession::identity_target(PlanningCandidateId candidate) const {
    return FakePressureTargetHandle{.generation = generation_, .index = candidate_index(candidate)};
}

FakePressureTargetHandle FakePressurePlanningSession::identity_target() const {
    require(candidates_.size() == 1, "fake capture pressure domain has multiple candidates");
    return FakePressureTargetHandle{.generation = generation_, .index = 0};
}

FakePressureTargetHandle
FakePressurePlanningSession::root_maximal_target(PlanningCandidateId candidate) {
    const std::uint32_t selected = candidate_index(candidate);
    populate_options(selected);
    Target maximal{
        .candidate_index = selected,
        .choices         = std::vector<std::uint16_t>(owners_.size(), 0),
        .root_maximal    = true,
    };
    for (std::size_t index = 0; index < owners_.size(); ++index) {
        maximal.choices[index] = static_cast<std::uint16_t>(options_[selected][index].size());
    }
    auto found = std::find_if(targets_.begin(), targets_.end(),
                              [&](const Target& target) { return same_target(target, maximal); });
    if (found != targets_.end()) {
        found->root_maximal = true;
        return FakePressureTargetHandle{
            .generation = generation_,
            .index      = static_cast<std::uint32_t>(found - targets_.begin()),
        };
    }
    maximal.stable_ordinal = static_cast<std::uint32_t>(targets_.size());
    targets_.push_back(std::move(maximal));
    return FakePressureTargetHandle{
        .generation = generation_,
        .index      = static_cast<std::uint32_t>(targets_.size() - 1U),
    };
}

std::optional<FakePressureTargetHandle>
FakePressurePlanningSession::maximal_target(PlanningCandidateId candidate) {
    ++program_->maximal_target_calls;
    if (refuse_maximal_targets) { return std::nullopt; }
    const auto target                   = root_maximal_target(candidate);
    targets_[target.index].root_maximal = false;
    return target;
}

FakePressurePlanningSession::Cursor
FakePressurePlanningSession::begin_construction(FakePressureTargetHandle handle, bool restore) {
    require(valid(handle) && !scratch_live_, "invalid fake construction parent");
    Cursor cursor;
    cursor.target     = targets_[handle.index];
    cursor.restore    = restore;
    cursor.generation = ++construction_generation_;
    populate_options(cursor.target.candidate_index);
    return cursor;
}

ninfer::runtime::PressureConstructionStep
FakePressurePlanningSession::next_construction_option(Cursor& cursor) {
    if (cursor.next_option < cursor.options.size()) {
        auto index = static_cast<std::uint32_t>(cursor.next_option++);
        return {.guidance = guidance_for(cursor.options[index]),
                .option   = {.cursor_generation = cursor.generation,
                             .scan_generation   = cursor.scan,
                             .index             = index}};
    }
    if (cursor.next_owner == owners_.size()) { return {.exhausted = true}; }
    const auto owner         = cursor.next_owner++;
    const auto& alternatives = options_[cursor.target.candidate_index][owner];
    const auto current       = cursor.target.choices[owner];
    for (std::size_t choice = cursor.restore ? 0 : 1; choice <= alternatives.size(); ++choice) {
        if (choice == current) { continue; }
        if (!cursor.restore && current != 0 &&
            (alternatives[current - 1].evicts_continuation ||
             !alternatives[choice - 1].evicts_continuation)) {
            continue;
        }
        Target child         = cursor.target;
        child.choices[owner] = static_cast<std::uint16_t>(choice);
        child.root_maximal   = false;
        cursor.options.push_back(std::move(child));
    }
    return {};
}

void FakePressurePlanningSession::choose_construction(
    Cursor& cursor, ninfer::runtime::PressureConstructionOptionId id) {
    require(id.cursor_generation == cursor.generation && id.scan_generation == cursor.scan &&
                id.index < cursor.options.size(),
            "stale fake construction option");
    cursor.target = cursor.options[id.index];
    cursor.options.clear();
    cursor.next_owner = cursor.next_option = 0;
    ++cursor.scan;
}

std::optional<FakePressureTargetHandle>
FakePressurePlanningSession::construction_target(const Cursor& cursor) {
    ++program_->construction_target_calls;
    auto found = std::find_if(targets_.begin(), targets_.end(), [&](const Target& other) {
        return same_target(other, cursor.target);
    });
    if (found == targets_.end()) {
        if (program_->refuse_construction_targets) { return std::nullopt; }
        // THE REAL BOUND, mirrored rather than derived: this fixture cannot include the model header that
        // owns `target_arena_maximum`, and the arithmetic here is only the fallback for a fake that has no
        // refusal knob set. If the model's sizing changes, this literal drifts -- the knob above is what
        // the truncation tests actually use.
        if (targets_.size() >= candidates_.size() + 1U + 4096U) { return std::nullopt; }
        auto target           = cursor.target;
        target.stable_ordinal = static_cast<std::uint32_t>(targets_.size());
        targets_.push_back(std::move(target));
        program_->pressure_target_count_peak =
            std::max(program_->pressure_target_count_peak, targets_.size());
        return FakePressureTargetHandle{.generation = generation_,
                                        .index = static_cast<std::uint32_t>(targets_.size() - 1)};
    }
    return FakePressureTargetHandle{.generation = generation_,
                                    .index = static_cast<std::uint32_t>(found - targets_.begin())};
}

ninfer::runtime::PressureTargetGuidance
FakePressurePlanningSession::guidance(FakePressureTargetHandle handle) {
    require(valid(handle) && !scratch_live_, "fake pressure guidance is stale");
    return guidance_for(targets_[handle.index]);
}

ninfer::runtime::PressureTargetGuidance
FakePressurePlanningSession::guidance_for(const Target& target) {
    populate_options(target.candidate_index);
    const FakeAdmissionCandidate& candidate = *candidates_[target.candidate_index];
    guidance_outcomes_.clear();
    std::vector<FakeTargetDecision> selected;
    std::uint32_t degradation_units = 0;
    std::uint32_t dropped           = 0;
    for (std::size_t index = 0; index < owners_.size(); ++index) {
        const std::uint16_t choice = target.choices[index];
        if (choice == 0) { continue; }
        const auto& alternatives = options_[target.candidate_index][index];
        require(choice <= alternatives.size(), "fake pressure guidance choice is invalid");
        const FakeTargetDecision& decision = alternatives[choice - 1U];
        selected.push_back(decision);
        degradation_units += decision.degradation_units;
        dropped += decision.dropped_checkpoints;
        guidance_outcomes_.push_back(ninfer::runtime::PressureOwnerOutcome{
            .owner               = owners_[index].id,
            .disposition         = decision.evicts_continuation ? VictimDisposition::Evicted
                                                                : VictimDisposition::Retained,
            .degradation_units   = decision.degradation_units,
            .dropped_checkpoints = decision.dropped_checkpoints,
        });
    }
    MaterializationMachineWork machine = candidate.identity.machine_work;
    const bool combined_copy_cancelled =
        program_->combined_target_cancels_pressure_copy && selected.size() > 1U &&
        std::none_of(selected.begin(), selected.end(),
                     [](const auto& decision) { return decision.evicts_continuation; });
    if (!selected.empty() && program_->pressure_target_immediate_ns_override) {
        const auto optimistic = machine.optimistic_candidate_transfers;
        set_fake_machine_costs(machine, 0, *program_->pressure_target_immediate_ns_override);
        machine.optimistic_candidate_transfers = optimistic;
    } else if (combined_copy_cancelled) {
        ++machine.pressure_transfers[2].payload_bytes;
    } else {
        for (const FakeTargetDecision& decision : selected) {
            machine.pressure_transfers[2].payload_bytes += decision.immediate_ns;
            ++machine.pressure_transfers[2].copy_operations;
        }
    }
    const std::size_t selected_units = program_->pressure_units(selected);
    const std::size_t remaining      = selected_units >= program_->required_pressure_actions
                                           ? 0
                                           : program_->required_pressure_actions - selected_units;
    return ninfer::runtime::PressureTargetGuidance{
        .physical =
            {
                .unsatisfied_constraints   = remaining == 0 ? 0U : 1U,
                .estimated_remaining_steps = static_cast<std::uint32_t>(remaining),
                .normalized_residual_q20   = static_cast<std::uint64_t>(remaining) << 20U,
            },
        .estimated_machine_work     = machine,
        .owner_outcomes             = guidance_outcomes_,
        .candidate                  = candidate_ids_[target.candidate_index],
        .stable_target_ordinal      = target.stable_ordinal,
        .degradation_units          = degradation_units,
        .dropped_checkpoints        = dropped,
        .source_mode                = candidate.source_mode,
        .recovery_estimate_complete = true,
    };
}

FakeAssessedPressureTarget FakePressurePlanningSession::assess(FakePressureTargetHandle handle) {
    require(valid(handle) && !scratch_live_, "fake pressure assessment is stale");
    if (program_->pressure_assessment_delay_us != 0) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(program_->pressure_assessment_delay_us));
    }
    ++program_->pressure_target_assessments;
    const Target& target = targets_[handle.index];
    populate_options(target.candidate_index);
    const FakeAdmissionCandidate& candidate = *candidates_[target.candidate_index];
    assessment_outcomes_.clear();
    assessment_impacts_.clear();
    assessment_recovery_work_.clear();
    assessment_recovery_work_.reserve(2U * owners_.size());
    std::vector<FakeTargetDecision> selected;
    std::uint32_t degradation_units = 0;
    std::uint32_t dropped           = 0;
    bool expandable                 = false;
    for (std::size_t index = 0; index < owners_.size(); ++index) {
        const std::uint16_t choice = target.choices[index];
        const auto& alternatives   = options_[target.candidate_index][index];
        if (choice == 0) {
            expandable = expandable || !alternatives.empty();
            continue;
        }
        require(choice <= alternatives.size(), "fake pressure target choice is invalid");
        const FakeTargetDecision& decision = alternatives[choice - 1U];
        selected.push_back(decision);
        degradation_units += decision.degradation_units;
        dropped += decision.dropped_checkpoints;
        assessment_outcomes_.push_back(ninfer::runtime::PressureOwnerOutcome{
            .owner               = owners_[index].id,
            .disposition         = decision.evicts_continuation ? VictimDisposition::Evicted
                                                                : VictimDisposition::Retained,
            .degradation_units   = decision.degradation_units,
            .dropped_checkpoints = decision.dropped_checkpoints,
        });
        const auto append_checkpoint_outcome = [&](CheckpointRef checkpoint, bool survives) {
            assessment_recovery_work_.push_back(
                fake_recovery_work(survives ? 0 : program_->pressure_checkpoint_recovery_ns));
            assessment_impacts_.push_back(ninfer::runtime::PressureCheckpointRecoveryImpact{
                .owner                = owners_[index].id,
                .checkpoint           = checkpoint,
                .target_recovery_work = std::span<const CheckpointRecoveryAlternativeWork>(
                    &assessment_recovery_work_.back(), 1),
                .survives = survives,
            });
        };
        const bool endpoint_survives =
            !decision.evicts_continuation && decision.dropped_checkpoints == 0;
        append_checkpoint_outcome(CheckpointRef{.kind     = owners_[index].shared
                                                                ? CheckpointKind::SharedStablePrefix
                                                                : CheckpointKind::SessionEndpoint,
                                                .frontier = program_->finish_frontier,
                                                .ordinal  = 0},
                                  endpoint_survives);
        if (!owners_[index].shared && program_->finish_with_rewrite) {
            append_checkpoint_outcome(CheckpointRef{.kind     = CheckpointKind::TurnClosure,
                                                    .frontier = program_->finish_frontier - 1U,
                                                    .ordinal  = 0},
                                      !decision.evicts_continuation);
        }
        if (!decision.evicts_continuation) { expandable = true; }
    }

    MaterializationMachineWork machine = candidate.identity.machine_work;
    const bool combined_copy_cancelled =
        program_->combined_target_cancels_pressure_copy && selected.size() > 1U &&
        std::none_of(selected.begin(), selected.end(),
                     [](const auto& decision) { return decision.evicts_continuation; });
    if (!selected.empty() && program_->pressure_target_immediate_ns_override) {
        const auto optimistic = machine.optimistic_candidate_transfers;
        set_fake_machine_costs(machine, 0, *program_->pressure_target_immediate_ns_override);
        machine.optimistic_candidate_transfers = optimistic;
    } else if (combined_copy_cancelled) {
        // The complete target removes a transfer required by each partial target.  This models
        // source Move replacing Fork, a later eviction cancelling an earlier D2H, or two physical
        // actions coalescing into one direct stage.  Exact target cost is therefore intentionally
        // non-monotonic along the search edge.
        ++machine.pressure_transfers[2].payload_bytes;
    } else {
        for (const FakeTargetDecision& decision : selected) {
            machine.pressure_transfers[2].payload_bytes += decision.immediate_ns;
            ++machine.pressure_transfers[2].copy_operations;
        }
    }
    const bool identity  = selected.empty();
    std::uint64_t digest = candidate.identity.assessment_digest;
    if (!identity) {
        digest = 1469598103934665603ULL;
        for (const std::uint16_t choice : target.choices) {
            digest ^= choice;
            digest *= 1099511628211ULL;
        }
        digest ^= target.candidate_index;
    }
    ninfer::runtime::PressureTargetAssessment assessment{
        .physical_status       = program_->target_feasible(selected)
                                     ? ninfer::runtime::MaterializationPhysicalStatus::Feasible
                                     : ninfer::runtime::MaterializationPhysicalStatus::Infeasible,
        .source_mode           = candidate.source_mode,
        .machine_work          = machine,
        .owner_outcomes        = assessment_outcomes_,
        .checkpoint_impacts    = assessment_impacts_,
        .candidate             = candidate_ids_[target.candidate_index],
        .stable_target_ordinal = target.stable_ordinal,
        .degradation_units     = degradation_units,
        .dropped_checkpoints   = dropped,
        .projection_work       = 1U + assessment_outcomes_.size(),
        .assessment_digest     = digest,
        .expandable            = expandable,
        .root_maximal          = target.root_maximal,
    };
    return FakeAssessedPressureTarget(handle, assessment);
}

FakePreparedPressureExpansion
FakePressurePlanningSession::prepare_expansion(FakePressureTargetHandle handle,
                                               std::uint32_t maximum_owners) {
    require(valid(handle) && !scratch_live_, "fake pressure expansion is stale");
    const Target& parent = targets_[handle.index];
    populate_options(parent.candidate_index);
    expansion_scratch_.clear();
    prepared_parent_    = handle.index;
    prepared_owner_end_ = static_cast<std::uint32_t>(std::min<std::size_t>(
        owners_.size(), static_cast<std::size_t>(parent.next_expansion_owner) + maximum_owners));
    for (std::size_t owner = parent.next_expansion_owner; owner < prepared_owner_end_; ++owner) {
        const std::uint16_t current = parent.choices[owner];
        const auto& alternatives    = options_[parent.candidate_index][owner];
        if (current == 0) {
            for (std::size_t choice = 1; choice <= alternatives.size(); ++choice) {
                Target child               = parent;
                child.choices[owner]       = static_cast<std::uint16_t>(choice);
                child.root_maximal         = false;
                child.next_expansion_owner = 0;
                if (std::none_of(expansion_scratch_.begin(), expansion_scratch_.end(),
                                 [&](const Target& prior) { return same_target(prior, child); })) {
                    expansion_scratch_.push_back(std::move(child));
                }
            }
        } else if (current <= alternatives.size() &&
                   !alternatives[current - 1U].evicts_continuation) {
            Target child               = parent;
            child.choices[owner]       = static_cast<std::uint16_t>(alternatives.size());
            child.root_maximal         = false;
            child.next_expansion_owner = 0;
            if (std::none_of(expansion_scratch_.begin(), expansion_scratch_.end(),
                             [&](const Target& prior) { return same_target(prior, child); })) {
                expansion_scratch_.push_back(std::move(child));
            }
        }
    }
    std::uint32_t new_count = 0;
    for (const Target& child : expansion_scratch_) {
        if (std::none_of(targets_.begin(), targets_.end(),
                         [&](const Target& prior) { return same_target(prior, child); })) {
            ++new_count;
        }
    }
    if (++scratch_generation_ == 0) { ++scratch_generation_; }
    scratch_live_ = true;
    return FakePreparedPressureExpansion(generation_, scratch_generation_, new_count);
}

FakePressureExpansionView
FakePressurePlanningSession::commit_expansion(FakePreparedPressureExpansion&& prepared) {
    require(scratch_live_ && prepared.generation == generation_ &&
                prepared.scratch_generation == scratch_generation_,
            "fake prepared expansion is stale");
    if (program_->pressure_optional_target_capacity) {
        const std::size_t maximum =
            candidates_.size() + 1U + *program_->pressure_optional_target_capacity;
        if (prepared.new_count > maximum - std::min(maximum, targets_.size())) {
            throw std::length_error("prepared pressure expansion exceeds the target arena");
        }
    }
    committed_children_.clear();
    std::uint32_t new_count = 0;
    for (Target& child : expansion_scratch_) {
        auto found          = std::find_if(targets_.begin(), targets_.end(),
                                           [&](const Target& prior) { return same_target(prior, child); });
        std::uint32_t index = 0;
        if (found == targets_.end()) {
            child.stable_ordinal = static_cast<std::uint32_t>(targets_.size());
            targets_.push_back(std::move(child));
            index = static_cast<std::uint32_t>(targets_.size() - 1U);
            ++new_count;
        } else {
            index = static_cast<std::uint32_t>(found - targets_.begin());
        }
        committed_children_.push_back(
            FakePressureTargetHandle{.generation = generation_, .index = index});
    }
    program_->pressure_target_count_peak =
        std::max(program_->pressure_target_count_peak, targets_.size());
    require(new_count == prepared.new_count, "fake expansion count changed before commit");
    expansion_scratch_.clear();
    scratch_live_                                   = false;
    targets_[prepared_parent_].next_expansion_owner = prepared_owner_end_;
    return FakePressureExpansionView{
        .children            = committed_children_,
        .new_canonical_count = new_count,
        .complete            = prepared_owner_end_ == owners_.size(),
    };
}

void FakePressurePlanningSession::discard_expansion(
    FakePreparedPressureExpansion&& prepared) noexcept {
    if (scratch_live_ && prepared.generation == generation_ &&
        prepared.scratch_generation == scratch_generation_) {
        expansion_scratch_.clear();
        scratch_live_ = false;
    }
}

PrefillWork FakePressurePlanningSession::shared_capture_split_prefill_work(
    const FakeAssessedPressureTarget&, const FakePreparedPrompt&,
    std::span<const std::uint32_t> frontiers) const {
    return PrefillWork{.attention_pairs = static_cast<std::uint64_t>(frontiers.size()) * 100U};
}

std::optional<FakeResourcePlan>
FakePressurePlanningSession::seal(FakeAssessedPressureTarget&& assessed, const FakePreparedPrompt&,
                                  ninfer::runtime::FinalScheduleIntent intent) {
    const FakePressureTargetHandle handle = assessed.target_;
    require(valid(handle) && !scratch_live_, "fake pressure seal is stale");
    const Target& target   = targets_[handle.index];
    const auto& assessment = assessed.assessment_;
    if (assessment.physical_status != ninfer::runtime::MaterializationPhysicalStatus::Feasible) {
        return std::nullopt;
    }
    assessed.target_.generation = 0;
    FakeResourcePlan plan(*candidates_[target.candidate_index], revision_);
    std::vector<std::uint64_t> action_ids;
    for (std::size_t index = 0; index < owners_.size(); ++index) {
        const std::uint16_t choice = target.choices[index];
        if (choice == 0) { continue; }
        const FakeTargetDecision& decision = options_[target.candidate_index][index][choice - 1U];
        action_ids.push_back(decision.id);
        if (owners_[index].shared) {
            plan.shared_actions.push_back(decision);
            plan.shared_owner_ids.push_back(owners_[index].shared_handle->id);
            plan.shared_planning_ids.push_back(owners_[index].id);
        } else {
            plan.private_actions.push_back(decision);
            plan.private_owner_ids.push_back(owners_[index].private_handle->id);
            plan.private_planning_ids.push_back(owners_[index].id);
        }
    }
    program_->selected_shared_capture_frontiers.assign(intent.shared_capture_frontiers.begin(),
                                                       intent.shared_capture_frontiers.end());
    program_->seal_attempts.push_back(std::move(action_ids));
    return plan;
}

bool FakePressurePlanningSession::try_claim_seal_window() noexcept { return true; }

void FakePressurePlanningSession::release_seal_window() noexcept {}

std::optional<FakeResourcePlan>
FakePressurePlanningSession::seal_capture(FakeAssessedPressureTarget&& assessed) {
    return seal(std::move(assessed), FakePreparedPrompt{}, {});
}

std::optional<FakeResourcePlan>
FakePressurePlanningSession::seal(FakeAssessedPressureTarget&& assessed) {
    return seal_capture(std::move(assessed));
}

FakePressurePlanningSession
FakeProgram::begin_pressure_planning(std::span<const FakeAdmissionCandidate* const> candidates,
                                     std::span<const PlanningCandidateId> candidate_ids,
                                     std::span<const FakeContinuationHandle* const> private_owners,
                                     std::span<const PlanningOwnerId> private_owner_ids,
                                     std::span<const FakeSharedPrefixHandle* const> shared_owners,
                                     std::span<const PlanningOwnerId> shared_owner_ids) {
    FakePressurePlanningSession session(*this, candidates, candidate_ids, private_owners,
                                        private_owner_ids, shared_owners, shared_owner_ids);
    session.refuse_maximal_targets = refuse_maximal_targets;
    return session;
}

struct FakeModelContract {
    using Program                    = FakeProgram;
    using PreparedPrompt             = FakePreparedPrompt;
    using RequestBasePlan            = FakeRequestBasePlan;
    using AdmissionCandidate         = FakeAdmissionCandidate;
    using ResourcePlan               = FakeResourcePlan;
    using PersistentBackfillProof    = FakePersistentBackfillProof;
    using SequenceHandle             = FakeSequenceHandle;
    using ContinuationHandle         = FakeContinuationHandle;
    using SharedPrefixHandle         = FakeSharedPrefixHandle;
    using CaptureOffer               = FakeCaptureOffer;
    using ContinuationSummary        = FakeContinuationSummary;
    using SharedPrefixSummary        = FakeSharedPrefixSummary;
    using CaptureAssessment          = FakeCaptureAssessment;
    using CapturePressurePlan        = FakeResourcePlan;
    using ActiveCaptureResult        = FakeActiveCaptureResult;
    using ContextTransactionProgress = FakeContextTransactionProgress;
    using MaterializationResult      = FakeMaterializationResult;
    using StartResult                = FakeStartResult;
    using FinishResult               = FakeFinishResult;
    using AbortResult                = FakeAbortResult;
    using PressureTargetHandle       = FakePressureTargetHandle;
    using AssessedPressureTarget     = FakeAssessedPressureTarget;
    using CommitResult               = FakeCommitResult;
    using DiscardResult              = FakeDiscardResult;
    using CacheSessionKey            = FakeCacheSessionKey;
};

using FakeManager = ninfer::runtime::ResourceManager<FakeModelContract>;

FakeManager make_manager(std::uint32_t lanes = 1, std::uint32_t private_capacity = 4,
                         std::uint32_t shared_capacity = 0, bool cache_enabled = true) {
    return FakeManager(lanes, private_capacity, shared_capacity, cache_enabled, 2,
                       test_cost_model());
}

struct ActiveRequest {
    LaneId lane;
    FakeSequenceHandle sequence;
};

ActiveRequest start_active(FakeManager& manager, FakeProgram& program, std::uint32_t content_key,
                           const FakeRequestBasePlan& base, std::uint64_t publication_order) {
    auto inspection =
        manager.inspect(program, FakePreparedPrompt{content_key}, base, publication_order);
    require(inspection.choice.has_value(), "request did not produce an admission choice");
    const LaneId lane   = inspection.choice->destination();
    const auto reserved = manager.reserve_materialization(program, std::move(*inspection.choice),
                                                          FakePreparedPrompt{content_key}, {});
    require(reserved == FakeManager::MaterializationReserveResult::Reserved,
            "request materialization was not reserved");
    auto outcome = [&]() -> FakeManager::MaterializationOutcome {
        auto progress = manager.progress_context_transaction(program, {});
        if (!std::holds_alternative<ContextTransactionInProgress>(progress)) {
            return std::get<FakeManager::MaterializationOutcome>(std::move(progress));
        }
        auto completed = manager.progress_context_transaction(program, {});
        return std::get<FakeManager::MaterializationOutcome>(std::move(completed));
    }();
    require(outcome.status == ContextTransactionStatus::Published && outcome.activation,
            "request materialization did not publish");
    auto activation                   = std::move(*outcome.activation);
    const FakeSequenceHandle sequence = activation.sequence();
    manager.adopt(program, std::move(activation));
    require(manager.lane_state(lane) == ninfer::runtime::LogicalLaneState::Active,
            "published lane was not adopted as active");
    return ActiveRequest{.lane = lane, .sequence = sequence};
}

void test_private_portfolio_loss_keeps_checkpoint_identity_fixed() {
    using ninfer::runtime::ContextPortfolioCheckpointValue;
    using ninfer::runtime::ContextPortfolioOwnerPolicy;
    using ninfer::runtime::ContextPortfolioValue;

    const std::array owners{
        ContextPortfolioOwnerPolicy{.owner                    = PlanningOwnerId{.value = 0},
                                    .private_retention_weight = 4},
    };
    const std::array checkpoints{
        ContextPortfolioCheckpointValue{
            .owner                = PlanningOwnerId{.value = 0},
            .rebuild_ns           = 1000,
            .baseline_recovery_ns = 100,
            .target_recovery_ns   = 100,
        },
        ContextPortfolioCheckpointValue{
            .owner                = PlanningOwnerId{.value = 0},
            .rebuild_ns           = 800,
            .baseline_recovery_ns = 100,
            .target_recovery_ns   = 800,
        },
    };
    ContextPortfolioValue value;
    const auto result = value.fold(owners, checkpoints);
    require(result.baseline_public_value == 0 && result.target_public_value == 0 &&
                result.private_transition_loss == 2800 && !result.saturated,
            "a surviving endpoint masked loss of an earlier private checkpoint");
}

void test_portfolio_demand_and_owner_aggregation() {
    using ninfer::runtime::ContextPortfolioCheckpointValue;
    using ninfer::runtime::ContextPortfolioOwnerPolicy;
    using ninfer::runtime::ContextPortfolioValue;

    {
        const std::array owners{ContextPortfolioOwnerPolicy{.owner = PlanningOwnerId{.value = 0}}};
        const std::array checkpoints{
            ContextPortfolioCheckpointValue{
                .owner                = PlanningOwnerId{.value = 0},
                .demand_mask          = 1,
                .rebuild_ns           = 1000,
                .baseline_recovery_ns = 200,
                .target_recovery_ns   = 500,
            },
            ContextPortfolioCheckpointValue{
                .owner                = PlanningOwnerId{.value = 0},
                .demand_mask          = 1,
                .rebuild_ns           = 800,
                .baseline_recovery_ns = 200,
                .target_recovery_ns   = 400,
            },
        };
        ContextPortfolioValue value;
        const auto result = value.fold(owners, checkpoints);
        require(result.baseline_public_value == 800 && result.target_public_value == 500 &&
                    result.private_transition_loss == 0,
                "nested checkpoints counted one empirical demand more than once");
    }

    {
        const std::array owners{
            ContextPortfolioOwnerPolicy{.owner                    = PlanningOwnerId{.value = 0},
                                        .private_retention_weight = 1},
            ContextPortfolioOwnerPolicy{.owner                    = PlanningOwnerId{.value = 1},
                                        .private_retention_weight = 4},
        };
        const std::array checkpoints{
            ContextPortfolioCheckpointValue{
                .owner                = PlanningOwnerId{.value = 0},
                .rebuild_ns           = 1000,
                .baseline_recovery_ns = 100,
                .target_recovery_ns   = 400,
            },
            ContextPortfolioCheckpointValue{
                .owner                = PlanningOwnerId{.value = 1},
                .rebuild_ns           = 1000,
                .baseline_recovery_ns = 100,
                .target_recovery_ns   = 200,
            },
        };
        ContextPortfolioValue value;
        const auto result = value.fold(owners, checkpoints);
        require(result.private_transition_loss == 700,
                "private checkpoint transition losses were not summed across owners");
    }
}

void test_shared_capture_subtracts_private_transition_loss() {
    using Planner = ninfer::runtime::SharedCapturePlanner<FakeModelContract>;

    FakeProgram program;
    program.required_pressure_actions             = 1;
    program.require_evictions                     = true;
    program.pressure_target_immediate_ns_override = 0;
    FakeCaptureAssessment capture{
        .shared_evidence     = ninfer::SharedCandidateEvidence::ExplicitBoundary,
        .publishes_shared    = true,
        .physically_feasible = false,
    };
    FakeContinuationHandle owner{7, 0};
    const std::array<const FakeContinuationHandle*, 1> private_owners{&owner};
    const std::array<PlanningOwnerId, 1> private_owner_ids{PlanningOwnerId{.value = 0}};
    const std::array<Planner::OwnerPolicy, 1> owner_policies{
        Planner::OwnerPolicy{.owner = PlanningOwnerId{.value = 0}, .private_retention_weight = 4},
    };
    const std::array<Planner::CheckpointPolicy, 1> checkpoint_policies{
        Planner::CheckpointPolicy{
            .owner                = PlanningOwnerId{.value = 0},
            .checkpoint           = CheckpointRef{.kind     = CheckpointKind::SessionEndpoint,
                                                  .frontier = 16,
                                                  .ordinal  = 0},
            .rebuild_ns           = 1000,
            .baseline_recovery_ns = 0,
        },
    };

    Planner planner;
    const auto result = planner.plan(program, test_cost_model(),
                                     Planner::Input{
                                         .capture              = &capture,
                                         .private_owners       = private_owners,
                                         .private_owner_ids    = private_owner_ids,
                                         .shared_owners        = {},
                                         .shared_owner_ids     = {},
                                         .owner_policies       = owner_policies,
                                         .checkpoint_policies  = checkpoint_policies,
                                         .candidate_rebuild_ns = 1000,
                                     });
    require(result && result->baseline_value == 0 && result->target_value == 1000 &&
                result->immediate_ns == 0 && result->net_gain == 600,
            "shared capture gain did not subtract the private capability transition loss");
}

void test_shared_capture_budget_bounds_committed_canonical_targets() {
    using Planner = ninfer::runtime::SharedCapturePlanner<FakeModelContract>;

    constexpr std::size_t private_owner_count = 16;
    constexpr std::size_t shared_owner_count  = 4;
    constexpr std::uint32_t target_budget     = 64;

    FakeProgram program;
    program.required_pressure_actions         = private_owner_count + shared_owner_count + 1U;
    program.pressure_optional_target_capacity = target_budget;

    std::array<FakeContinuationHandle, private_owner_count> private_handles;
    std::array<const FakeContinuationHandle*, private_owner_count> private_owners;
    std::array<PlanningOwnerId, private_owner_count> private_owner_ids;
    for (std::size_t index = 0; index < private_owner_count; ++index) {
        private_handles[index] = FakeContinuationHandle(static_cast<std::uint32_t>(index + 1U), 0);
        private_owners[index]  = &private_handles[index];
        private_owner_ids[index] = PlanningOwnerId{.value = static_cast<std::uint32_t>(index)};
    }

    std::array<FakeSharedPrefixHandle, shared_owner_count> shared_handles;
    std::array<const FakeSharedPrefixHandle*, shared_owner_count> shared_owners;
    std::array<PlanningOwnerId, shared_owner_count> shared_owner_ids;
    for (std::size_t index = 0; index < shared_owner_count; ++index) {
        shared_handles[index].id = static_cast<std::uint32_t>(index + 1U);
        shared_owners[index]     = &shared_handles[index];
        shared_owner_ids[index] =
            PlanningOwnerId{.value = static_cast<std::uint32_t>(private_owner_count + index)};
    }

    const FakeCaptureAssessment capture{
        .shortlist_key       = FakeShortlistKey{.digest = 91, .frontier = 64},
        .publishes_shared    = true,
        .physically_feasible = false,
    };
    Planner planner;
    const auto result = planner.plan(program, test_cost_model(),
                                     Planner::Input{
                                         .capture           = &capture,
                                         .private_owners    = private_owners,
                                         .private_owner_ids = private_owner_ids,
                                         .shared_owners     = shared_owners,
                                         .shared_owner_ids  = shared_owner_ids,
                                         .target_budget     = target_budget,
                                     });
    require(!result, "bounded infeasible shared-capture search unexpectedly found a plan");
    require(program.pressure_target_count_peak <= target_budget,
            "shared-capture search committed more canonical targets than its budget");
}

void test_equal_lower_bound_does_not_short_circuit_tie_break() {
    using Planner = ninfer::runtime::MaterializationPlanner<FakeModelContract>;

    FakeProgram program;
    program.required_pressure_actions         = 1;
    program.pressure_action_immediate_ns      = 0;
    program.pressure_action_degradation_units = 0;

    FakeAdmissionCandidate incumbent;
    set_fake_machine_costs(incumbent.identity.machine_work, 1'000'000'000, 1'000'000'000);
    incumbent.identity.machine_work.candidate_transfers[2].copy_operations = 1;
    incumbent.identity.physical_status   = ninfer::runtime::MaterializationPhysicalStatus::Feasible;
    incumbent.identity.source_mode       = PrivateSourceMode::ConsumeToActive;
    incumbent.identity.assessment_digest = 11;

    FakeAdmissionCandidate tied_pressure;
    set_fake_machine_costs(tied_pressure.identity.machine_work, 1'000'000'000, 1'000'000'000);
    tied_pressure.identity.physical_status =
        ninfer::runtime::MaterializationPhysicalStatus::Infeasible;
    tied_pressure.identity.source_mode       = PrivateSourceMode::ConsumeToActive;
    tied_pressure.identity.expandable        = true;
    tied_pressure.identity.assessment_digest = 22;

    std::array<Planner::CandidateInput, 2> candidates{
        Planner::CandidateInput{.candidate               = &incumbent,
                                .id                      = PlanningCandidateId{.value = 0},
                                .stable_ordinal          = 0,
                                .current_session_binding = false},
        Planner::CandidateInput{.candidate               = &tied_pressure,
                                .id                      = PlanningCandidateId{.value = 1},
                                .stable_ordinal          = 1,
                                .current_session_binding = true},
    };
    FakeContinuationHandle owner{7, 0};
    const std::array<const FakeContinuationHandle*, 1> private_owners{&owner};
    const std::array<PlanningOwnerId, 1> private_owner_ids{PlanningOwnerId{.value = 0}};
    const std::array<ninfer::runtime::MaterializationOwnerPolicy, 1> owner_policy{
        ninfer::runtime::MaterializationOwnerPolicy{.owner = PlanningOwnerId{.value = 0}},
    };
    const std::array<ninfer::runtime::MaterializationCheckpointPolicy, 1> checkpoint_policy{
        ninfer::runtime::MaterializationCheckpointPolicy{
            .owner      = PlanningOwnerId{.value = 0},
            .checkpoint = CheckpointRef{.kind     = CheckpointKind::SessionEndpoint,
                                        .frontier = 16,
                                        .ordinal  = 0},
            .rebuild_ns = 100,
        },
    };

    Planner planner;
    const auto logical_goal = [](PlanningCandidateId, PrivateSourceMode,
                                 std::span<const ninfer::runtime::PressureOwnerOutcome>)
        -> std::optional<Planner::LogicalGoal> {
        return Planner::LogicalGoal{.publication_slot = 0};
    };
    const auto pressure_inputs = [&]() -> Planner::PressureInputs {
        return Planner::PressureInputs{
            .private_owners    = private_owners,
            .private_owner_ids = private_owner_ids,
            .shared_owners     = {},
            .shared_owner_ids  = {},
            .owner_policy      = owner_policy,
            .checkpoint_policy = checkpoint_policy,
        };
    };
    auto result = planner.plan(program, FakePreparedPrompt{}, test_cost_model(), candidates, 0,
                               pressure_inputs, logical_goal, Planner::Clock::now());

    require(result && result->candidate == PlanningCandidateId{.value = 1} &&
                program.pressure_planning_sessions == 1,
            "equal lower bound bypassed the pressure target that wins the stable tie-break");
}

void test_machine_cost_changes_selection_without_changing_physical_assessment() {
    using Planner = ninfer::runtime::MaterializationPlanner<FakeModelContract>;

    FakeProgram program;
    FakeAdmissionCandidate prefill_candidate;
    prefill_candidate.identity.machine_work.remaining_prefill_work.tokens = 10;
    prefill_candidate.identity.physical_status =
        ninfer::runtime::MaterializationPhysicalStatus::Feasible;
    prefill_candidate.identity.assessment_digest = 31;

    FakeAdmissionCandidate transfer_candidate;
    transfer_candidate.identity.machine_work.candidate_transfers[1] = {.payload_bytes   = 100,
                                                                       .copy_operations = 1};
    transfer_candidate.identity.machine_work.optimistic_candidate_transfers[1] = {
        .payload_bytes = 100, .copy_operations = 1};
    transfer_candidate.identity.physical_status =
        ninfer::runtime::MaterializationPhysicalStatus::Feasible;
    transfer_candidate.identity.assessment_digest = 32;

    const std::array<Planner::CandidateInput, 2> candidates{
        Planner::CandidateInput{.candidate      = &prefill_candidate,
                                .id             = PlanningCandidateId{.value = 0},
                                .stable_ordinal = 0},
        Planner::CandidateInput{.candidate      = &transfer_candidate,
                                .id             = PlanningCandidateId{.value = 1},
                                .stable_ordinal = 1},
    };
    const auto pressure_inputs = [] { return Planner::PressureInputs{}; };
    const auto logical_goal    = [](PlanningCandidateId, PrivateSourceMode,
                                 std::span<const ninfer::runtime::PressureOwnerOutcome>)
        -> std::optional<Planner::LogicalGoal> {
        return Planner::LogicalGoal{.publication_slot = 0};
    };

    auto prefill_expensive                 = test_cost_model();
    prefill_expensive.prefill.token_ns_q32 = 100U * ninfer::runtime::kContextCostQ32One;
    Planner first_planner;
    const auto first =
        first_planner.plan(program, FakePreparedPrompt{}, prefill_expensive, candidates, 0,
                           pressure_inputs, logical_goal, Planner::Clock::now());

    auto transfer_expensive                 = test_cost_model();
    transfer_expensive.prefill.token_ns_q32 = ninfer::runtime::kContextCostQ32One;
    for (auto& direction : transfer_expensive.transfer) {
        direction.ns_per_byte_q32 = 100U * ninfer::runtime::kContextCostQ32One;
    }
    Planner second_planner;
    const auto second =
        second_planner.plan(program, FakePreparedPrompt{}, transfer_expensive, candidates, 0,
                            pressure_inputs, logical_goal, Planner::Clock::now());

    require(first && first->candidate == PlanningCandidateId{.value = 1} && second &&
                second->candidate == PlanningCandidateId{.value = 0} &&
                prefill_candidate.identity.physical_status ==
                    ninfer::runtime::MaterializationPhysicalStatus::Feasible &&
                transfer_candidate.identity.physical_status ==
                    ninfer::runtime::MaterializationPhysicalStatus::Feasible,
            "machine cost policy changed physical assessment or failed to change selection");
}

void test_candidate_search_prefers_deep_reuse_without_eviction() {
    using Planner = ninfer::runtime::MaterializationPlanner<FakeModelContract>;

    FakeProgram program;
    program.required_pressure_actions         = 2;
    program.eviction_pressure_action_units    = 2;
    program.pressure_action_immediate_ns      = 1'000'000;
    program.pressure_action_degradation_units = 1;
    program.pressure_assessment_delay_us      = 1'000;
    program.pressure_checkpoint_recovery_ns   = 8'000'000'000ULL;

    FakeAdmissionCandidate root;
    set_fake_machine_costs(root.identity.machine_work, 8'000'000'000ULL, 8'000'000'000ULL);
    root.identity.physical_status   = ninfer::runtime::MaterializationPhysicalStatus::Infeasible;
    root.identity.source_mode       = PrivateSourceMode::ConsumeToActive;
    root.identity.expandable        = true;
    root.identity.assessment_digest = 101;

    FakeAdmissionCandidate reuse;
    reuse.value.reusable_prompt_tokens = 55'048;
    reuse.private_source_id            = 1;
    set_fake_machine_costs(reuse.identity.machine_work, 100'000'000, 100'000'000);
    reuse.identity.machine_work.reused_prompt_tokens = 55'048;
    reuse.identity.physical_status   = ninfer::runtime::MaterializationPhysicalStatus::Infeasible;
    reuse.identity.source_mode       = PrivateSourceMode::ConsumeToActive;
    reuse.identity.expandable        = true;
    reuse.identity.assessment_digest = 202;

    std::array<Planner::CandidateInput, 2> candidates{
        Planner::CandidateInput{.candidate               = &root,
                                .id                      = PlanningCandidateId{.value = 0},
                                .stable_ordinal          = 0,
                                .current_session_binding = false},
        Planner::CandidateInput{.candidate               = &reuse,
                                .id                      = PlanningCandidateId{.value = 1},
                                .stable_ordinal          = 1,
                                .current_session_binding = true},
    };
    std::array<FakeContinuationHandle, 3> owner_handles{
        FakeContinuationHandle{1, 0},
        FakeContinuationHandle{2, 0},
        FakeContinuationHandle{3, 0},
    };
    const std::array<const FakeContinuationHandle*, 3> private_owners{
        &owner_handles[0], &owner_handles[1], &owner_handles[2]};
    const std::array<PlanningOwnerId, 3> private_owner_ids{
        PlanningOwnerId{.value = 0}, PlanningOwnerId{.value = 1}, PlanningOwnerId{.value = 2}};
    const std::array<ninfer::runtime::MaterializationOwnerPolicy, 3> owner_policy{
        ninfer::runtime::MaterializationOwnerPolicy{.owner           = PlanningOwnerId{.value = 0},
                                                    .retention_class = RetentionClass::LiveSession,
                                                    .private_retention_weight = 16},
        ninfer::runtime::MaterializationOwnerPolicy{.owner           = PlanningOwnerId{.value = 1},
                                                    .retention_class = RetentionClass::LiveSession,
                                                    .private_retention_weight = 16},
        ninfer::runtime::MaterializationOwnerPolicy{.owner           = PlanningOwnerId{.value = 2},
                                                    .retention_class = RetentionClass::LiveSession,
                                                    .private_retention_weight = 16},
    };
    const std::array<ninfer::runtime::MaterializationCheckpointPolicy, 3> checkpoint_policy{
        ninfer::runtime::MaterializationCheckpointPolicy{
            .owner      = PlanningOwnerId{.value = 0},
            .checkpoint = CheckpointRef{.kind     = CheckpointKind::SessionEndpoint,
                                        .frontier = 16,
                                        .ordinal  = 0},
            .rebuild_ns = 8'000'000'000ULL},
        ninfer::runtime::MaterializationCheckpointPolicy{
            .owner      = PlanningOwnerId{.value = 1},
            .checkpoint = CheckpointRef{.kind     = CheckpointKind::SessionEndpoint,
                                        .frontier = 16,
                                        .ordinal  = 0},
            .rebuild_ns = 8'000'000'000ULL},
        ninfer::runtime::MaterializationCheckpointPolicy{
            .owner      = PlanningOwnerId{.value = 2},
            .checkpoint = CheckpointRef{.kind     = CheckpointKind::SessionEndpoint,
                                        .frontier = 16,
                                        .ordinal  = 0},
            .rebuild_ns = 8'000'000'000ULL},
    };

    Planner planner;
    const auto pressure_inputs = [&]() -> Planner::PressureInputs {
        return Planner::PressureInputs{
            .private_owners    = private_owners,
            .private_owner_ids = private_owner_ids,
            .shared_owners     = {},
            .shared_owner_ids  = {},
            .owner_policy      = owner_policy,
            .checkpoint_policy = checkpoint_policy,
        };
    };
    const auto logical_goal = [](PlanningCandidateId, PrivateSourceMode,
                                 std::span<const ninfer::runtime::PressureOwnerOutcome>)
        -> std::optional<Planner::LogicalGoal> {
        return Planner::LogicalGoal{.publication_slot = 0};
    };
    auto result = planner.plan(program, FakePreparedPrompt{}, test_cost_model(), candidates, 0,
                               pressure_inputs, logical_goal, Planner::Clock::now());

    require(result && result->plan && result->candidate == PlanningCandidateId{.value = 1},
            "shallow Root pressure path starved the cheaper reuse candidate");
    require(result->plan->private_actions.size() == 2 &&
                std::none_of(
                    result->plan->private_actions.begin(), result->plan->private_actions.end(),
                    [](const FakeTargetDecision& action) { return action.evicts_continuation; }),
            "one-step eviction outranked the multi-step preserving reuse closure");
}

void test_feasible_identity_expands_when_pressure_can_remove_copy() {
    using Planner = ninfer::runtime::MaterializationPlanner<FakeModelContract>;

    FakeProgram program;
    program.pressure_action_immediate_ns          = 0;
    program.pressure_action_degradation_units     = 1;
    program.pressure_target_immediate_ns_override = 100'000'000;

    FakeAdmissionCandidate candidate;
    set_fake_machine_costs(candidate.identity.machine_work, 100'000'000, 1'000'000'000);
    candidate.identity.machine_work.candidate_transfers[2].copy_operations = 1;
    candidate.identity.pressure_may_change_machine_work                    = true;
    candidate.identity.physical_status   = ninfer::runtime::MaterializationPhysicalStatus::Feasible;
    candidate.identity.source_mode       = PrivateSourceMode::ConsumeToActive;
    candidate.identity.assessment_digest = 17;

    const std::array<Planner::CandidateInput, 1> candidates{
        Planner::CandidateInput{.candidate               = &candidate,
                                .id                      = PlanningCandidateId{.value = 0},
                                .stable_ordinal          = 0,
                                .current_session_binding = false},
    };
    FakeContinuationHandle owner{9, 0};
    const std::array<const FakeContinuationHandle*, 1> private_owners{&owner};
    const std::array<PlanningOwnerId, 1> private_owner_ids{PlanningOwnerId{.value = 0}};
    const std::array<ninfer::runtime::MaterializationOwnerPolicy, 1> owner_policy{
        ninfer::runtime::MaterializationOwnerPolicy{.owner = PlanningOwnerId{.value = 0}},
    };
    const std::array<ninfer::runtime::MaterializationCheckpointPolicy, 1> checkpoint_policy{
        ninfer::runtime::MaterializationCheckpointPolicy{
            .owner      = PlanningOwnerId{.value = 0},
            .checkpoint = CheckpointRef{.kind     = CheckpointKind::SessionEndpoint,
                                        .frontier = 16,
                                        .ordinal  = 0},
            .rebuild_ns = 100,
        },
    };

    Planner planner;
    const auto pressure_inputs = [&]() -> Planner::PressureInputs {
        return Planner::PressureInputs{
            .private_owners    = private_owners,
            .private_owner_ids = private_owner_ids,
            .shared_owners     = {},
            .shared_owner_ids  = {},
            .owner_policy      = owner_policy,
            .checkpoint_policy = checkpoint_policy,
        };
    };
    const auto logical_goal = [](PlanningCandidateId, PrivateSourceMode,
                                 std::span<const ninfer::runtime::PressureOwnerOutcome>)
        -> std::optional<Planner::LogicalGoal> {
        return Planner::LogicalGoal{.publication_slot = 0};
    };
    auto result = planner.plan(program, FakePreparedPrompt{}, test_cost_model(), candidates, 0,
                               pressure_inputs, logical_goal, Planner::Clock::now());

    require(result && result->diagnostics.predicted_now_ns == 100'000'000 &&
                result->diagnostics.selected_degradation_units == 1 &&
                program.pressure_planning_sessions == 1 && !program.seal_attempts.empty() &&
                program.seal_attempts.back() == std::vector<std::uint64_t>{1009},
            "feasible identity suppressed a cheaper complete pressure target");
}

void test_dominating_identity_does_not_build_pressure_graph() {
    using Planner = ninfer::runtime::MaterializationPlanner<FakeModelContract>;

    FakeProgram program;
    FakeAdmissionCandidate candidate;
    set_fake_machine_costs(candidate.identity.machine_work, 100'000'000, 100'000'000);
    candidate.identity.physical_status   = ninfer::runtime::MaterializationPhysicalStatus::Feasible;
    candidate.identity.source_mode       = PrivateSourceMode::ConsumeToActive;
    candidate.identity.assessment_digest = 23;
    const std::array<Planner::CandidateInput, 1> candidates{
        Planner::CandidateInput{.candidate               = &candidate,
                                .id                      = PlanningCandidateId{.value = 0},
                                .stable_ordinal          = 0,
                                .current_session_binding = false},
    };

    bool pressure_inputs_built = false;
    const auto pressure_inputs = [&]() -> Planner::PressureInputs {
        pressure_inputs_built = true;
        return {};
    };
    const auto logical_goal = [](PlanningCandidateId, PrivateSourceMode,
                                 std::span<const ninfer::runtime::PressureOwnerOutcome>)
        -> std::optional<Planner::LogicalGoal> {
        return Planner::LogicalGoal{.publication_slot = 0};
    };

    Planner planner;
    auto result = planner.plan(program, FakePreparedPrompt{}, test_cost_model(), candidates, 0,
                               pressure_inputs, logical_goal, Planner::Clock::now());
    require(result &&
                result->diagnostics.stop_reason == ninfer::MaterializationStopReason::NoPressure &&
                !pressure_inputs_built && program.pressure_planning_sessions == 0,
            "dominating identity eagerly constructed the pressure graph");
}

FakeFinishResult finish_active(FakeManager& manager, FakeProgram& program, ActiveRequest request,
                               std::uint32_t frontier = 16) {
    program.finish_frontier = frontier;
    manager.mark_terminal_pending(request.lane);
    return manager.finish(program, request.lane, request.sequence);
}

void test_root_lifecycle_and_prefix_reuse() {
    FakeManager manager = make_manager(1, 2);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 7, make_base(7), 1);
    require(program.pressure_planning_sessions == 0,
            "no-pressure root admission created a pressure planning session");
    const FakeFinishResult finish = finish_active(manager, program, first);
    require(finish.status == ConsumeStatus::Consumed &&
                finish.disposition == FinishDisposition::Catalogued,
            "terminal continuation was not catalogued");
    require(manager.lane_state(first.lane) == ninfer::runtime::LogicalLaneState::Free,
            "terminal lane did not return to Free");

    auto reuse = manager.inspect(program, FakePreparedPrompt{7}, make_base(7), 2);
    require(reuse.readiness == Readiness::Ready && reuse.choice,
            "catalogued endpoint was not reusable");
    require(reuse.choice->summary().reusable_prompt_tokens == 16,
            "endpoint reuse frontier was not selected");
    program.abort_start = true;
    const auto status   = manager.reserve_materialization(program, std::move(*reuse.choice),
                                                          FakePreparedPrompt{7}, {});
    require(status == FakeManager::MaterializationReserveResult::Stale &&
                program.started_source_id == first.sequence.id,
            "selected endpoint did not reach the sealed Program plan");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Catalogued,
            "failed start did not roll back its logical source claim");
}

// THE SPLIT, end to end through `ResourceManager`: every Catalogued entry is scanned with
// `Program::prefix_split`, and `best_prefix_split` folds them into the request's diagnostics. Asserts values
// that DIFFER by prompt, so a fake (or a manager) that returns constants cannot pass both halves: the
// unrelated prompt must read 0 tokens over the same entries the related prompt reads 16 over.
// THE SESSION FIELDS' OWN SEMANTICS, on a fresh manager so the catalog holds exactly one session slot with
// two entries (endpoint 16, rewrite 15) and the per-frontier refusal can make them disagree.
//
// Two things this pins that the pass-3 assertions could not:
//   * `session_endpoint_skip` answers for the ENDPOINT, not for whichever entry of the slot was visited last.
//     Replacing its `index.checkpoint == own.summary.endpoint->ref` test with `true` survives the older cases,
//     because there every entry of the slot agreed.
//   * `session_cell_skip`'s offered-is-sticky rule. An endpoint that was offered followed by a refused
//     rewrite must still read 1: the cell DID produce a candidate. Without the rule it reports the rewrite's
//     refusal instead, which is the confusion the field exists to remove.
// THE ERASURE COUNTERS MUST BE WIRED, not merely present -- the cell-clear counters once shipped incrementing
// at nine sites and never copying out, so `/stats` served a hardcoded zero that read like a clean finding. This
// drives the ordinary consume path and requires the counter to move, and pins the occupancy/capacity pair.
void test_session_erase_counters_are_wired() {
    FakeManager manager = make_manager(1, 4);
    FakeProgram program;
    const FakeCacheSessionKey session{21};
    const auto materialize = [&](std::uint64_t order) {
        FakeRequestBasePlan base = make_base(21, session, RetentionClass::LiveSession);
        auto inspection = manager.inspect(program, FakePreparedPrompt{21}, base, order);
        require(inspection.choice.has_value(), "erase fixture produced no choice");
        const LaneId lane = inspection.choice->destination();
        require(manager.reserve_materialization(program, std::move(*inspection.choice),
                                                FakePreparedPrompt{21}, {}) ==
                    FakeManager::MaterializationReserveResult::Reserved,
                "erase fixture was not reserved");
        auto outcome = [&]() -> FakeManager::MaterializationOutcome {
            auto progress = manager.progress_context_transaction(program, {});
            if (!std::holds_alternative<ContextTransactionInProgress>(progress)) {
                return std::get<FakeManager::MaterializationOutcome>(std::move(progress));
            }
            auto completed = manager.progress_context_transaction(program, {});
            return std::get<FakeManager::MaterializationOutcome>(std::move(completed));
        }();
        require(outcome.status == ContextTransactionStatus::Published && outcome.activation,
                "erase fixture did not publish");
        auto activation                   = std::move(*outcome.activation);
        const FakeSequenceHandle sequence = activation.sequence();
        manager.adopt(program, std::move(activation));
        return ActiveRequest{.lane = lane, .sequence = sequence};
    };

    const ActiveRequest seed = materialize(1);
    (void)finish_active(manager, program, seed, 16);
    RuntimeStats before;
    manager.populate_runtime_stats(program, before);
    require(before.session_index_capacity_cells == 4,
            "the session index must report its capacity, or a full index is indistinguishable from an empty one");
    // EXACTLY ONE, not "at least one": `occupied := capacity` was a surviving mutant, and a >= test cannot
    // see it. The conversation has published one continuation into an index of four.
    require(before.session_index_occupied_cells == 1,
            "one published continuation must occupy exactly one session index entry of the four");

    // The conversation's next turn: same key, matching digest, so the source is CONSUMED as the active source.
    const ActiveRequest next = materialize(2);
    (void)next;
    RuntimeStats after;
    manager.populate_runtime_stats(program, after);
    require(after.session_erasures_consume > before.session_erasures_consume,
            "consuming a session's own continuation must count as a session-index erasure: this is the path "
            "that leaves the next turn with no cell and a 23k-token re-prefill");
    require(after.session_erasures_eviction == before.session_erasures_eviction,
            "a consume must not be counted as an eviction -- the two have different fixes");
}

// M2: THE EVICTION PATH MUST COUNT AS AN EVICTION, not as a consume. The split between the two erasure
// reasons is the whole point of the pair (an eviction is the route that leaves a conversation with no cell;
// a consume is the ordinary path that then republishes), and until this test the Eviction site had NO
// coverage at all -- swapping it to Consume survived every other case, because nothing drove an eviction
// through the manager. `require_evictions` forces a plan whose every alternative evicts.
void test_session_erase_counts_an_eviction_as_eviction() {
    FakeManager manager = make_manager(1, 3);
    FakeProgram program;
    const FakeCacheSessionKey victim_session{31};
    const FakeCacheSessionKey keeper_session{32};
    const ActiveRequest victim = start_active(
        manager, program, 31, make_base(31, victim_session, RetentionClass::LiveSession), 1);
    (void)finish_active(manager, program, victim, 16);
    const ActiveRequest keeper = start_active(
        manager, program, 32, make_base(32, keeper_session, RetentionClass::LiveSession), 2);
    (void)finish_active(manager, program, keeper, 16);

    RuntimeStats before;
    manager.populate_runtime_stats(program, before);
    require(before.session_erasures_eviction == 0 && before.session_erasures_consume == 0,
            "the fixture erased a session entry before any pressure was applied");

    program.required_pressure_actions = 1;
    program.require_evictions         = true;
    auto inspection = manager.inspect(
        program, FakePreparedPrompt{33},
        make_base(33, FakeCacheSessionKey{33}, RetentionClass::LiveSession), 3);
    require(inspection.choice.has_value(), "the eviction fixture produced no choice");
    require(manager.reserve_materialization(program, std::move(*inspection.choice),
                                            FakePreparedPrompt{33}, {}) ==
                FakeManager::MaterializationReserveResult::Reserved,
            "the eviction fixture was not reserved");
    auto outcome = [&]() -> FakeManager::MaterializationOutcome {
        auto progress = manager.progress_context_transaction(program, {});
        if (!std::holds_alternative<ContextTransactionInProgress>(progress)) {
            return std::get<FakeManager::MaterializationOutcome>(std::move(progress));
        }
        auto completed = manager.progress_context_transaction(program, {});
        return std::get<FakeManager::MaterializationOutcome>(std::move(completed));
    }();
    require(outcome.status == ContextTransactionStatus::Published && outcome.activation,
            "the eviction fixture did not publish");
    auto activation                   = std::move(*outcome.activation);
    const FakeSequenceHandle sequence = activation.sequence();
    manager.adopt(program, std::move(activation));

    RuntimeStats after;
    manager.populate_runtime_stats(program, after);
    require(after.session_erasures_eviction > before.session_erasures_eviction,
            "an eviction erased a session entry and was not counted as an Eviction: swapping the two reasons "
            "at the Evicted call site must fail here");
    require(after.session_erasures_consume == before.session_erasures_consume,
            "an eviction was counted as a Consume -- the two reasons have different fixes and must not mix");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Vacant,
            "the eviction fixture did not evict the victim, so it proves nothing about that path");
    (void)keeper;
    (void)sequence;
}

// M4/M5: THE CENSUS MUST COUNT WHAT IT CLAIMS. `host_state_checkpoints_*` splits host-resident checkpoints
// by whether a session cell or an active lane edge can still reach them, and it is the measurement that
// decides whether a reclaim policy has a population at all. It had NO test: swapping the two counts, or
// hard-coding `orphaned` to 0, both survived.
//
// The shape is deliberately ASYMMETRIC -- one anchored against two unanchored -- because a 1/1 fixture would
// pass under the swap as well. Three continuations for ONE conversation, published in order: its cell holds
// the newest, and the two it superseded stay Catalogued as anonymous cache.
void test_host_state_census_counts_unanchored_checkpoints() {
    FakeManager manager = make_manager(1, 4);
    FakeProgram program;
    program.host_residency = ninfer::runtime::ReplicaResidency::HostOnly;
    const FakeCacheSessionKey session{41};
    for (std::uint32_t i = 0; i < 3; ++i) {
        const ActiveRequest turn = start_active(
            manager, program, 41 + i, make_base(41 + i, session, RetentionClass::LiveSession), 1 + i);
        (void)finish_active(manager, program, turn, 16 + i);
    }
    RuntimeStats stats;
    manager.populate_runtime_stats(program, stats);
    require(stats.host_state_checkpoints_reachable == 1,
            "exactly one host-resident checkpoint is the conversation's cell; the census must count it once");
    require(stats.host_state_checkpoints_unanchored == 2,
            "the two superseded continuations are host-resident and unanchored; counting them as 0 (or as "
            "reachable) is the mutant this catches");
    // CONTROL: with residency DeviceOnly the same three continuations hold nothing on host, so both counts
    // must be zero -- otherwise the numbers above could come from the catalog rather than from residency.
    FakeProgram cold;
    cold.host_residency = ninfer::runtime::ReplicaResidency::DeviceOnly;
    FakeManager cold_manager = make_manager(1, 4);
    for (std::uint32_t i = 0; i < 3; ++i) {
        const ActiveRequest turn = start_active(
            cold_manager, cold, 41 + i, make_base(41 + i, session, RetentionClass::LiveSession), 1 + i);
        (void)finish_active(cold_manager, cold, turn, 16 + i);
    }
    RuntimeStats cold_stats;
    cold_manager.populate_runtime_stats(cold, cold_stats);
    require(cold_stats.host_state_checkpoints_reachable == 0 &&
                cold_stats.host_state_checkpoints_unanchored == 0,
            "device-only checkpoints must count zero: the census measures residency, not catalog size");
}

void test_session_endpoint_reason_is_per_frontier() {
    const auto refusal_diagnostics = [](std::uint32_t refuse_frontier, bool allow_shortlist,
                                        std::uint32_t prompt_key, std::uint32_t identity_tag = 0) {
        FakeManager manager = make_manager(1, 4);
        FakeProgram program;
        const FakeCacheSessionKey session{13};
        const auto materialize = [&](std::uint64_t order, std::uint32_t key, bool shortlist) {
            FakeRequestBasePlan base = make_base(13, session, RetentionClass::LiveSession);
            base.refuse_frontier     = refuse_frontier;
            base.allow_shortlist     = shortlist;
            base.identity_tag        = identity_tag;
            auto inspection = manager.inspect(program, FakePreparedPrompt{key}, base, order);
            require(inspection.choice.has_value(), "per-frontier fixture produced no choice");
            const LaneId lane = inspection.choice->destination();
            require(manager.reserve_materialization(program, std::move(*inspection.choice),
                                                    FakePreparedPrompt{key}, {}) ==
                        FakeManager::MaterializationReserveResult::Reserved,
                    "per-frontier fixture was not reserved");
            auto outcome = [&]() -> FakeManager::MaterializationOutcome {
                auto progress = manager.progress_context_transaction(program, {});
                if (!std::holds_alternative<ContextTransactionInProgress>(progress)) {
                    return std::get<FakeManager::MaterializationOutcome>(std::move(progress));
                }
                auto completed = manager.progress_context_transaction(program, {});
                return std::get<FakeManager::MaterializationOutcome>(std::move(completed));
            }();
            require(outcome.status == ContextTransactionStatus::Published && outcome.activation,
                    "per-frontier fixture did not publish");
            auto activation                   = std::move(*outcome.activation);
            const FakeSequenceHandle sequence = activation.sequence();
            manager.adopt(program, std::move(activation));
            // THE E INVARIANT, ENFORCED ON EVERY CALL THIS HELPER MAKES. `preserving_alternatives_assessed` is
        // ungated and `feasible_preserving_alternatives` is its goal-gated subset -- both are incremented in
        // the same function, the gated one INSIDE the ungated condition -- so the first must never be the
        // smaller. The transposition that shipped on 2026-10-01 put the goal-less count into `assessed` and
        // left the arena flag holding `assessed != 0`; nothing asserted this, and the field's first bug was
        // invisible to every E-specific check because there were none.
        return std::pair{ActiveRequest{.lane = lane, .sequence = sequence}, outcome.diagnostics};
        };

        auto [seed, seed_diagnostics] = materialize(1, 13, true);
        (void)seed_diagnostics;
        // The rewrite is requested only AFTER materializing: setting it before makes the seal throw
        // `selected checkpoint outcome is unknown or duplicated`.
        program.finish_with_rewrite = true;
        (void)finish_active(manager, program, seed, 16);
        program.finish_with_rewrite = false;

        auto [reuse, diagnostics] = materialize(2, prompt_key, allow_shortlist);
        (void)reuse;
        return diagnostics;
    };

    // ORDER DEPENDENCY, stated rather than left to be discovered: `endpoint_any` (a mutant making the
    // endpoint test always true) dies here only because `rebuild_prefix_index` appends the endpoint BEFORE the
    // rewrite, so "last writer wins" and "the endpoint wins" differ. Reversing those two appends would disarm
    // this assertion silently. Anchors are appended later still, which is why adding one would make it
    // order-proof -- not done, because the order is what the code guarantees today.
    //
    // The endpoint's own key is refused; the shallower rewrite is offered. The cell WAS offered, so
    // `session_cell_skip` must stay 1, and the endpoint's refusal must show in `session_endpoint_skip`.
    const auto endpoint_refused = refusal_diagnostics(16, true, 13);
    require(endpoint_refused.session_cell_skip == 1 && endpoint_refused.session_endpoint_skip == 5,
            "an offered cell with a refused endpoint must read cell=1 (offered is sticky) and endpoint=5");
    // The rewrite's key is refused; the endpoint is offered. Both fields read 1.
    const auto rewrite_refused = refusal_diagnostics(15, true, 13);
    require(rewrite_refused.session_cell_skip == 1 && rewrite_refused.session_endpoint_skip == 1,
            "a refused shallower entry must not overwrite the endpoint's own reason");

    // The two remaining reachable codes. Without these the field reads the provisional 2 ("no index entry at
    // all") when either site is dropped, which is a wrong diagnosis no assertion could catch -- the fifth
    // review pass deleted each note site and the suite stayed green.
    const auto no_shortlist = refusal_diagnostics(0, /*allow_shortlist=*/false, 13);
    require(no_shortlist.session_cell_skip == 3 && no_shortlist.session_endpoint_skip == 3,
            "a request offering no shortlist key must report code 3 in both fields");
    const auto admission_refused = refusal_diagnostics(0, true, /*prompt_key=*/12);
    require(admission_refused.session_cell_skip == 8 && admission_refused.session_endpoint_skip == 8,
            "an entry whose key matches but whose source admission refuses it must report code 8");

    // Code 4 is the LAST site cheaply reachable: the identity tag is compared before the digests, so a
    // non-zero tag on the request side is a mismatch the fixture can produce without new lanes or knobs.
    const auto tag_mismatch = refusal_diagnostics(0, true, 13, /*identity_tag=*/7);
    require(tag_mismatch.session_cell_skip == 4 && tag_mismatch.session_endpoint_skip == 4,
            "a request whose identity tag differs from the stored entry's must report code 4");
}

void test_prefix_split_diagnostics_follow_catalog() {
    FakeManager manager = make_manager(1, 4);
    FakeProgram program;
    const auto materialize_with = [&](FakePreparedPrompt prompt, const FakeRequestBasePlan& base,
                                      std::uint64_t order) {
        auto inspection = manager.inspect(program, prompt, base, order);
        require(inspection.choice.has_value(), "split fixture produced no choice");
        const LaneId lane   = inspection.choice->destination();
        const auto reserved = manager.reserve_materialization(program, std::move(*inspection.choice),
                                                              std::move(prompt), {});
        require(reserved == FakeManager::MaterializationReserveResult::Reserved,
                "split fixture was not reserved");
        auto outcome = [&]() -> FakeManager::MaterializationOutcome {
            auto progress = manager.progress_context_transaction(program, {});
            if (!std::holds_alternative<ContextTransactionInProgress>(progress)) {
                return std::get<FakeManager::MaterializationOutcome>(std::move(progress));
            }
            auto completed = manager.progress_context_transaction(program, {});
            return std::get<FakeManager::MaterializationOutcome>(std::move(completed));
        }();
        require(outcome.status == ContextTransactionStatus::Published && outcome.activation,
                "split fixture did not publish");
        auto activation                   = std::move(*outcome.activation);
        const FakeSequenceHandle sequence = activation.sequence();
        manager.adopt(program, std::move(activation));
        return std::pair{ActiveRequest{.lane = lane, .sequence = sequence}, outcome.diagnostics};
    };
    const auto materialize = [&](std::uint32_t key, std::uint64_t order) {
        return materialize_with(FakePreparedPrompt{key}, make_base(key), order);
    };

    // Empty catalog: nothing scanned, nothing matched -- the denominator is what tells this from a miss.
    auto [first, empty] = materialize(7, 1);
    require(empty.split_entries == 0 && empty.split_best_tokens == 0,
            "empty catalog reported split entries");
    // THE RED CONTROL FOR THE SENTINEL COLLISION (review, 2026-09-28). With no session cell, `session_slot` is
    // the no-cell sentinel -- and that value was ALSO `kInvalidCatalogSlot`, which is what every unoccupied
    // `prefix_index_` tail entry defaults to. So `index.slot != session_slot` was false for each of them and
    // this field read 9, "failed validation", on every request without a session key (the Bash classifier's
    // traffic included) while the documented 0 was unreachable. This assertion FAILS on that code -- with the
    // caveat that the two guards it rests on are redundant (`!index.occupied` alone would close the collision),
    // so it pins their union, not each of them separately.
    require(empty.session_cell_skip == 0,
            "a request with no session cell must report no-cell (0), not a skip reason: the no-cell sentinel "
            "collided with the invalid-catalog-slot sentinel that unoccupied index entries carry");
    program.finish_with_rewrite = true;
    (void)finish_active(manager, program, first, 16);  // key 7: ledger 16, endpoint 16, rewrite 15
    program.finish_with_rewrite = false;
    auto [second, one] = materialize(9, 2);
    require(one.split_entries == 1 && one.split_best_tokens == 0 && one.split_best_restorable == 0 &&
                !one.split_identity_ok,
            "unrelated prompt matched a foreign catalog entry");
    (void)finish_active(manager, program, second, 24);  // key 9: ledger 24, endpoint 24

    const std::uint64_t calls_before = program.prefix_split_calls;
    auto [third, related]            = materialize(7, 3);
    require(program.prefix_split_calls - calls_before == 2,
            "split did not scan every Catalogued entry exactly once");
    require(related.split_entries == 2, "split denominator does not count both entries");
    require(related.split_best_tokens == 16 && related.split_best_restorable == 16 &&
                related.split_identity_ok,
            "related prompt did not read its own entry's match and restorable frontier");
    // THE MANAGER'S WIRING of the two fields added with the split's localisation. They are computed by
    // `best_prefix_split` (covered in test_materialization_budget) and copied here; without this the copy could
    // be dropped and nothing would fail.
    require(related.split_best_stored == 16 && related.split_ended_by == 1,
            "the manager did not carry the deepest entry's ledger length and match-end discriminator");
    // WHICH LEDGER. This request carries no session key, so a private entry is ANOTHER session's (2) -- and
    // without the tag a divergence window cannot be attributed to the entry whose refusal is the question.
    require(related.split_best_source == 2 && related.split_best_frontier == 16,
            "the deepest match must name its ledger (2 = another session's here) and that entry's resume point");
    // `third` is left unfinished by the block above; the lane has to come back before anything else can be
    // admitted, since this fixture has exactly one.
    (void)finish_active(manager, program, third, 32);

    // THE INSTRUMENT'S OWN FIELDS, MADE ABLE TO FAIL. `0` is the default, so asserting 0 proves nothing about
    // the wiring -- with only that assertion, deleting the offered-note or either diagnostics copy left the
    // suite green, and the third review pass built those three mutants and all survived. Each case below takes
    // a value a dropped line would change.
    const FakeCacheSessionKey session{7};
    auto [seed, seed_diagnostics] = materialize_with(
        FakePreparedPrompt{11}, make_base(11, session, RetentionClass::LiveSession), 4);
    (void)seed_diagnostics;
    (void)finish_active(manager, program, seed, 16);

    auto [reuse, reuse_diagnostics] =
        materialize_with(FakePreparedPrompt{11}, make_base(11, session, RetentionClass::LiveSession), 5);
    require(reuse_diagnostics.split_best_source == 1,
            "a session-keyed request's own continuation must be tagged as its own ledger (1)");
    require(reuse_diagnostics.session_cell_offered && reuse_diagnostics.session_cell_skip == 1 &&
                reuse_diagnostics.session_endpoint_skip == 1 && reuse_diagnostics.session_cell_frontier == 16,
            "a same-session reuse must read offered=1 in BOTH session fields, with the cell's frontier -- "
            "0 here means the note or the copy is missing");
    (void)finish_active(manager, program, reuse, 16);



    FakeRequestBasePlan key_mismatch = make_base(11, session, RetentionClass::LiveSession);
    key_mismatch.shortlist_digest    = 99;
    auto [refused, refused_diagnostics] = materialize_with(FakePreparedPrompt{11}, key_mismatch, 6);
    require(refused_diagnostics.session_cell_skip == 5 && refused_diagnostics.session_endpoint_skip == 5,
            "a same-session key mismatch must report digest-mismatch (5) in both fields, not a default");
    (void)finish_active(manager, program, refused, 16);

    // A DIVERGENT match: the fake caps the match below the entry's ledger, which is the only way it can
    // produce one -- and the probe fields are why the cap exists.
    auto [diverged, diverged_diagnostics] =
        materialize_with(FakePreparedPrompt{11, /*match_limit=*/8}, make_base(11), 7);
    require(diverged_diagnostics.split_ended_by == 0 && diverged_diagnostics.split_probe_index == 8 &&
                diverged_diagnostics.split_best_stored == 16,
            "a capped match must report Diverged, the probe index at the stop, and the entry's own ledger");
    // The cap is 8, and the fake's synthetic frontiers put 5 at message 1 (a User turn): a manager that
    // dropped the pass-through would report index 0 / offset 0 / role 0 (System) here.
    require(diverged_diagnostics.split_message_index == 1 && diverged_diagnostics.split_message_offset == 3 &&
                diverged_diagnostics.split_message_role ==
                    static_cast<std::uint8_t>(ninfer::ChatRole::User) &&
                !diverged_diagnostics.split_past_last_message,
            "the divergence's message index, offset and role must reach the diagnostics, not their defaults");

    (void)diverged;

}

void test_stale_revision_is_retryable() {
    FakeManager manager = make_manager();
    FakeProgram program;
    auto inspection = manager.inspect(program, FakePreparedPrompt{1}, make_base(1), 1);
    require(inspection.choice.has_value(), "root choice was not produced");
    const std::uint64_t start_calls = program.start_calls;
    program.invalidate_resources();
    const auto status = manager.reserve_materialization(program, std::move(*inspection.choice),
                                                        FakePreparedPrompt{1}, {});
    require(status == FakeManager::MaterializationReserveResult::Stale,
            "revision mismatch was not reported as retryable stale work");
    require(program.start_calls == start_calls,
            "known-stale plan was incorrectly passed into Program start");
    require(manager.lane_state(LaneId{0}) == ninfer::runtime::LogicalLaneState::Free,
            "known-stale plan changed logical lane state");
}

void test_materialization_abort_preserves_source() {
    FakeManager manager = make_manager(1, 2);
    FakeProgram program;
    const ActiveRequest seed = start_active(manager, program, 5, make_base(5), 1);
    (void)finish_active(manager, program, seed);

    auto inspection = manager.inspect(program, FakePreparedPrompt{5}, make_base(5), 2);
    require(inspection.choice && inspection.choice->summary().reusable_prompt_tokens == 16,
            "abort test did not select its private source");
    program.abort_progress = true;
    require(manager.reserve_materialization(program, std::move(*inspection.choice),
                                            FakePreparedPrompt{5}, {}) ==
                FakeManager::MaterializationReserveResult::Reserved,
            "abort test could not reserve materialization");
    auto progress = manager.progress_context_transaction(program, {});
    auto outcome  = std::get<FakeManager::MaterializationOutcome>(std::move(progress));
    require(outcome.status == ContextTransactionStatus::Aborted && !outcome.activation,
            "cancelled materialization did not abort before publication");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Catalogued,
            "aborted Move did not restore its source visibility");

    program.abort_progress = false;
    program.abort_start    = true;
    auto retry             = manager.inspect(program, FakePreparedPrompt{5}, make_base(5), 3);
    require(retry.choice && retry.choice->summary().reusable_prompt_tokens == 16,
            "restored source was not reusable after abort");
    (void)manager.reserve_materialization(program, std::move(*retry.choice), FakePreparedPrompt{5},
                                          {});
    require(program.started_source_id == seed.sequence.id,
            "abort restored the wrong source capability");
}

void test_committed_victim_survives_transaction_abort() {
    FakeManager manager = make_manager(1, 2);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 10, make_base(10), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 20, make_base(20), 2);
    (void)finish_active(manager, program, second);

    auto inspection = manager.inspect(program, FakePreparedPrompt{30}, make_base(30), 3);
    require(inspection.choice.has_value(), "full catalog did not produce an eviction closure");
    program.abort_progress = true;
    require(manager.reserve_materialization(program, std::move(*inspection.choice),
                                            FakePreparedPrompt{30}, {}) ==
                FakeManager::MaterializationReserveResult::Reserved,
            "evicting materialization was not reserved");
    auto progress = manager.progress_context_transaction(program, {});
    auto outcome  = std::get<FakeManager::MaterializationOutcome>(std::move(progress));
    require(outcome.status == ContextTransactionStatus::Aborted,
            "pressure transaction did not take the abort path");
    const std::uint32_t catalogued =
        (manager.catalog_state(0) == FakeManager::CatalogState::Catalogued ? 1U : 0U) +
        (manager.catalog_state(1) == FakeManager::CatalogState::Catalogued ? 1U : 0U);
    require(catalogued == 1,
            "committed victim eviction was incorrectly rolled back with request-local abort");
}

void test_uncommitted_pressure_acknowledgement_is_not_degradation() {
    FakeManager manager = make_manager(1, 2);
    FakeProgram program;
    const ActiveRequest seed = start_active(manager, program, 40, make_base(40), 1);
    (void)finish_active(manager, program, seed);

    program.required_pressure_actions = 1;
    auto inspection = manager.inspect(program, FakePreparedPrompt{41}, make_base(41), 2);
    require(inspection.choice.has_value(),
            "pressure-abort test did not select a preserving pressure target");
    program.abort_progress = true;
    require(manager.reserve_materialization(program, std::move(*inspection.choice),
                                            FakePreparedPrompt{41}, {}) ==
                FakeManager::MaterializationReserveResult::Reserved,
            "pressure-abort test could not reserve materialization");
    require(program.started_action_ids.size() == 1 &&
                program.started_action_ids.front() == 1000U + seed.sequence.id,
            "pressure-abort test selected an eviction instead of preserving pressure");

    auto progress = manager.progress_context_transaction(program, {});
    auto outcome  = std::get<FakeManager::MaterializationOutcome>(std::move(progress));
    RuntimeStats stats;
    manager.populate_runtime_stats(program, stats);
    require(outcome.status == ContextTransactionStatus::Aborted &&
                manager.catalog_state(0) == FakeManager::CatalogState::Catalogued,
            "uncommitted pressure acknowledgement changed owner availability");
    require(stats.pressure_private_owners_degraded == 0 &&
                stats.pressure_private_owners_evicted == 0 &&
                stats.pressure_checkpoints_dropped == 0,
            "uncommitted pressure acknowledgement was counted as a degradation");
    require(stats.pressure_searches == 1,
            "accepted pressure plan was hidden when its request later aborted");
}

void test_aborted_source_selection_does_not_create_hit_history() {
    FakeManager manager = make_manager(1, 3);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 61, make_base(61), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 62, make_base(62), 2);
    (void)finish_active(manager, program, second);

    auto reuse = manager.inspect(program, FakePreparedPrompt{61}, make_base(61), 3);
    require(reuse.choice && reuse.choice->summary().reusable_prompt_tokens == 16,
            "cancelled-hit test did not select its exact source");
    program.abort_progress = true;
    require(manager.reserve_materialization(program, std::move(*reuse.choice),
                                            FakePreparedPrompt{61}, {}) ==
                FakeManager::MaterializationReserveResult::Reserved,
            "cancelled-hit test could not reserve exact reuse");
    auto progress = manager.progress_context_transaction(program, {});
    auto outcome  = std::get<FakeManager::MaterializationOutcome>(std::move(progress));
    require(outcome.status == ContextTransactionStatus::Aborted,
            "cancelled-hit test unexpectedly published its request");

    program.abort_progress            = false;
    program.required_pressure_actions = 1;
    program.require_evictions         = true;
    auto pressure = manager.inspect(program, FakePreparedPrompt{63}, make_base(63), 4);
    require(pressure.choice.has_value(), "cancelled-hit test could not plan pressure");
    program.abort_start = true;
    (void)manager.reserve_materialization(program, std::move(*pressure.choice),
                                          FakePreparedPrompt{63}, {});
    require(program.started_action_ids.size() == 1 &&
                program.started_action_ids.front() == 2000U + first.sequence.id,
            "aborted source selection incorrectly biased later retention policy");
}

// #10 / #14 stage two. An unplannable request used to be reported as `TemporarilyBlocked` whatever
// the engine's state, so when the capacity it needed was occupied by something no owner could free, the
// request waited out its deadline for ever and blocked every request behind it (the 2026-09-25 wedge).
// A block is only temporary when something can end it -- an occupied lane can finish -- so with every
// lane Free and the plan finding nothing, the verdict is final.
//
// The control matters as much as the assertion: this must NOT pass against a manager that simply never
// blocks anything, so the same request is re-inspected while a lane is occupied and must then be
// *temporary*.
void test_unplannable_request_with_no_active_lane_is_permanently_infeasible() {
    FakeManager manager = make_manager(2, 3);
    FakeProgram program;
    program.required_pressure_actions = 1;  // the plan must reclaim something; nothing exists to reclaim

    // No lane has been started, so every lane is Free: the request is classed feasible against total
    // capacity (isolated_feasible) yet the plan can place it nowhere, and nothing is occupied that
    // could finish and change that. This is #10's condition, and it must be final rather than a wait.
    FakeRequestBasePlan idle_base = make_base(77);
    idle_base.isolated_feasible   = true;
    auto idle = manager.inspect(program, FakePreparedPrompt{77}, idle_base, 1);
    require(idle.readiness == Readiness::PermanentlyInfeasible && !idle.choice,
            "an unplannable request with every lane free was reported as merely blocked");

    // Control, taken afterwards because establishing a lane requires an admittable request: with a lane
    // occupied the same shape must remain a *temporary* block, or the assertion above would pass
    // against a manager that never blocks anything.
    program.required_pressure_actions = 0;
    const FakeCacheSessionKey session{1};
    const ActiveRequest active = start_active(
        manager, program, 9, make_base(9, session, RetentionClass::LiveSession), 2);
    program.required_pressure_actions = 1;
    FakeRequestBasePlan busy_base = make_base(78);
    busy_base.isolated_feasible   = true;
    auto blocked = manager.inspect(program, FakePreparedPrompt{78}, busy_base, 3);
    require(blocked.readiness == Readiness::TemporarilyBlocked && !blocked.choice,
            "the same request with a lane occupied must remain a temporary block");
    (void)finish_active(manager, program, active);
}

void test_retained_source_is_protected_until_terminal() {
    FakeManager manager = make_manager(2, 3);
    FakeProgram program;
    const FakeCacheSessionKey first_session{1};
    const FakeCacheSessionKey second_session{2};
    const ActiveRequest seed = start_active(
        manager, program, 9, make_base(9, first_session, RetentionClass::LiveSession), 1);
    (void)finish_active(manager, program, seed);

    const ActiveRequest fork = start_active(
        manager, program, 9, make_base(9, second_session, RetentionClass::LiveSession), 2);
    require(program.started_source_mode == PrivateSourceMode::Retain,
            "different-session source was destructively moved");

    program.required_pressure_actions = 1;
    auto blocked = manager.inspect(program, FakePreparedPrompt{77}, make_base(77), 3);
    require(blocked.readiness == Readiness::TemporarilyBlocked && !blocked.choice,
            "active retained source was exposed as a pressure victim");

    (void)manager.abort(program, fork.lane, fork.sequence);
    program.abort_start = true;
    auto available      = manager.inspect(program, FakePreparedPrompt{77}, make_base(77), 4);
    require(available.choice.has_value(),
            "terminal release did not return retained source to pressure policy");
    (void)manager.reserve_materialization(program, std::move(*available.choice),
                                          FakePreparedPrompt{77}, {});
    require(!program.started_action_ids.empty(),
            "released source did not participate in the sealed pressure plan");
}

void test_session_publication_order_controls_tied_source() {
    FakeManager manager = make_manager(2, 3);
    FakeProgram program;
    const FakeCacheSessionKey session{42};
    const FakeRequestBasePlan base = make_base(42, session, RetentionClass::LiveSession, true);

    const ActiveRequest older = start_active(manager, program, 42, base, 10);
    const ActiveRequest newer = start_active(manager, program, 42, base, 20);
    (void)finish_active(manager, program, newer, 16);
    (void)finish_active(manager, program, older, 16);

    auto next = manager.inspect(program, FakePreparedPrompt{42}, base, 30);
    require(next.choice && next.choice->summary().reusable_prompt_tokens == 16,
            "same-session endpoint candidates were not reusable");
    program.abort_start = true;
    (void)manager.reserve_materialization(program, std::move(*next.choice), FakePreparedPrompt{42},
                                          {});
    require(program.started_source_id == newer.sequence.id,
            "older out-of-order finish displaced the current session binding on a tied cost");

    program.abort_start             = false;
    const ActiveRequest replacement = start_active(
        manager, program, 99, make_base(99, session, RetentionClass::LiveSession, true), 40);
    (void)finish_active(manager, program, replacement, 16);
    require(program.released_continuations.empty(),
            "session publication synchronously released an old physical continuation");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Catalogued &&
                manager.catalog_state(1) == FakeManager::CatalogState::Catalogued &&
                manager.catalog_state(2) == FakeManager::CatalogState::Catalogued,
            "session replacement did not retain its old binding as anonymous cache");
}

void test_canonical_pressure_starts_with_disposable_owner() {
    FakeManager manager = make_manager(1, 3);
    FakeProgram program;
    const ActiveRequest disposable = start_active(
        manager, program, 1, make_base(1, std::nullopt, RetentionClass::Disposable), 1);
    (void)finish_active(manager, program, disposable);
    const ActiveRequest live = start_active(
        manager, program, 2, make_base(2, FakeCacheSessionKey{2}, RetentionClass::LiveSession), 2);
    (void)finish_active(manager, program, live);

    program.required_pressure_actions = 1;
    auto inspection = manager.inspect(program, FakePreparedPrompt{3}, make_base(3), 3);
    require(inspection.choice.has_value(), "canonical pressure did not find a feasible prefix");
    program.abort_start = true;
    (void)manager.reserve_materialization(program, std::move(*inspection.choice),
                                          FakePreparedPrompt{3}, {});
    require(program.started_action_ids.size() == 1 &&
                program.started_action_ids.front() == 1000U + disposable.sequence.id,
            "canonical pressure did not degrade Disposable before LiveSession");
}

void test_pressure_tries_every_preserving_alternative_before_eviction() {
    FakeManager manager = make_manager(1, 2);
    FakeProgram program;
    const ActiveRequest seed = start_active(manager, program, 15, make_base(15), 1);
    (void)finish_active(manager, program, seed);

    program.required_pressure_actions     = 1;
    program.private_pressure_alternatives = 2;
    program.required_action_id            = 11000U + seed.sequence.id;
    auto inspection = manager.inspect(program, FakePreparedPrompt{25}, make_base(25), 2);
    require(inspection.choice.has_value(), "second preserving pressure alternative was skipped");

    program.abort_start = true;
    (void)manager.reserve_materialization(program, std::move(*inspection.choice),
                                          FakePreparedPrompt{25}, {});
    require(program.started_action_ids.size() == 1 &&
                program.started_action_ids.front() == *program.required_action_id,
            "pressure escalated before trying the feasible preserving alternative");
}

void test_cumulative_owner_target_closes_pressure_without_eviction() {
    FakeManager manager = make_manager(1, 2);
    FakeProgram program;
    program.finish_with_rewrite = true;
    const ActiveRequest seed    = start_active(manager, program, 31, make_base(31), 1);
    (void)finish_active(manager, program, seed);

    program.required_pressure_actions         = 1;
    program.include_cumulative_private_target = true;
    program.required_action_id                = 5000U + seed.sequence.id;
    auto inspection = manager.inspect(program, FakePreparedPrompt{32}, make_base(32), 2);
    require(inspection.choice.has_value(),
            "cumulative checkpoint-drop and spill owner target was unreachable");

    program.abort_start = true;
    (void)manager.reserve_materialization(program, std::move(*inspection.choice),
                                          FakePreparedPrompt{32}, {});
    require(program.started_action_ids.size() == 1 &&
                program.started_action_ids.front() == *program.required_action_id,
            "planner replaced a feasible cumulative owner target with eviction");

    // THE RESCUE BRANCH, WHICH THIS SCENARIO IS THE ONE THAT REACHES. `maximal_target` has a single
    // caller -- the rescue at `materialization_planner.h` -- and a full target arena must make it
    // GIVE UP, not throw. An earlier version of this file claimed the branch was unreachable in the
    // fake and left it untested; that was false, and the denominator it leaned on
    // (`maximal_target_calls`) was never read by anything. It is read here.
    {
        FakeManager rescue_manager = make_manager(1, 2);
        FakeProgram rescue_program;
        rescue_program.finish_with_rewrite = true;
        const ActiveRequest rescue_seed =
            start_active(rescue_manager, rescue_program, 31, make_base(31), 1);
        (void)finish_active(rescue_manager, rescue_program, rescue_seed);
        rescue_program.required_pressure_actions         = 1;
        rescue_program.include_cumulative_private_target = true;
        rescue_program.required_action_id                = 5000U + rescue_seed.sequence.id;
        rescue_program.refuse_maximal_targets            = true;
        bool rescue_threw                                = false;
        try {
            auto rescue_inspection =
                rescue_manager.inspect(rescue_program, FakePreparedPrompt{32}, make_base(32), 2);
            if (rescue_inspection.choice.has_value()) {
                (void)rescue_manager.reserve_materialization(rescue_program,
                                                             std::move(*rescue_inspection.choice),
                                                             FakePreparedPrompt{32}, {});
            }
        } catch (const std::exception&) {
            rescue_threw = true;
        }
        RuntimeStats rescue_stats;
        rescue_manager.populate_runtime_stats(rescue_program, rescue_stats);

        require(rescue_program.maximal_target_calls > 0,
                "THE RESCUE BRANCH WAS NOT REACHED, so this arm measures the scenario and not the "
                "change -- `maximal_target` was never called");
        require(!rescue_threw,
                "A FULL TARGET ARENA THREW FROM THE RESCUE BRANCH: this is the 2026-09-28 HTTP 500 plus "
                "worker recovery, and the rescue is the call site that must degrade instead");
        require(rescue_stats.pressure_target_arena_truncations > 0,
                "the rescue branch's truncation was not counted, so a capacity ceiling that stops the "
                "search reads exactly like a healthy short search");
    }
}

void test_two_owners_jointly_close_pressure() {
    FakeManager manager = make_manager(1, 3);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 41, make_base(41), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 42, make_base(42), 2);
    (void)finish_active(manager, program, second);

    program.required_pressure_actions = 2;
    auto inspection = manager.inspect(program, FakePreparedPrompt{43}, make_base(43), 3);
    require(inspection.choice.has_value(), "joint two-owner pressure target was unreachable");

    program.abort_start = true;
    (void)manager.reserve_materialization(program, std::move(*inspection.choice),
                                          FakePreparedPrompt{43}, {});
    std::sort(program.started_action_ids.begin(), program.started_action_ids.end());
    const std::vector<std::uint64_t> expected{
        1000U + first.sequence.id,
        1000U + second.sequence.id,
    };
    require(program.started_action_ids == expected,
            "planner did not combine preserving targets from two owners");
}

void test_materialization_result_is_validated_before_any_adoption() {
    FakeManager manager = make_manager(1, 3);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 141, make_base(141), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 142, make_base(142), 2);
    (void)finish_active(manager, program, second);

    program.required_pressure_actions   = 2;
    program.malform_last_private_victim = true;
    auto inspection = manager.inspect(program, FakePreparedPrompt{143}, make_base(143), 3);
    require(inspection.choice.has_value(), "malformed-result fixture found no pressure plan");
    require(manager.reserve_materialization(program, std::move(*inspection.choice),
                                            FakePreparedPrompt{143}, {}) ==
                FakeManager::MaterializationReserveResult::Reserved,
            "malformed-result fixture could not reserve materialization");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Claimed &&
                manager.catalog_state(1) == FakeManager::CatalogState::Claimed,
            "malformed-result fixture did not claim both owners");

    bool rejected = false;
    try {
        (void)manager.progress_context_transaction(program, {});
    } catch (const std::logic_error&) { rejected = true; }
    require(rejected, "malformed materialization result was accepted");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Claimed &&
                manager.catalog_state(1) == FakeManager::CatalogState::Claimed,
            "malformed materialization result partially adopted an earlier victim");
}

void test_materialization_result_binds_exact_checkpoint_identity() {
    FakeManager manager = make_manager(1, 2);
    FakeProgram program;
    program.finish_with_rewrite = true;
    const ActiveRequest seed    = start_active(manager, program, 145, make_base(145), 1);
    (void)finish_active(manager, program, seed);

    program.required_pressure_actions           = 1;
    program.include_cumulative_private_target   = true;
    program.required_action_id                  = 5000U + seed.sequence.id;
    program.malform_private_checkpoint_identity = true;
    auto inspection = manager.inspect(program, FakePreparedPrompt{146}, make_base(146), 2);
    require(inspection.choice.has_value(), "checkpoint-identity fixture found no pressure plan");
    require(manager.reserve_materialization(program, std::move(*inspection.choice),
                                            FakePreparedPrompt{146}, {}) ==
                FakeManager::MaterializationReserveResult::Reserved,
            "checkpoint-identity fixture could not reserve materialization");

    bool rejected = false;
    try {
        (void)manager.progress_context_transaction(program, {});
    } catch (const std::logic_error&) { rejected = true; }
    require(rejected, "same-count checkpoint substitution was accepted");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Claimed,
            "checkpoint substitution partially mutated its logical owner");
}

void test_materialization_result_is_adopted_by_owner_identity() {
    FakeManager manager = make_manager(1, 3);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 151, make_base(151), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 152, make_base(152), 2);
    (void)finish_active(manager, program, second);

    program.required_pressure_actions = 2;
    program.reverse_pressure_results  = true;
    const ActiveRequest published     = start_active(manager, program, 153, make_base(153), 3);
    require(manager.catalog_state(0) == FakeManager::CatalogState::Catalogued &&
                manager.catalog_state(1) == FakeManager::CatalogState::Catalogued,
            "reordered materialization results changed victim availability");
    (void)finish_active(manager, program, published);

    program.required_pressure_actions = 0;
    {
        auto reuse = manager.inspect(program, FakePreparedPrompt{151}, make_base(151), 4);
        require(reuse.choice && reuse.choice->summary().reusable_prompt_tokens == 16,
                "reordered materialization result attached the first summary to another owner");
    }
    {
        auto reuse = manager.inspect(program, FakePreparedPrompt{152}, make_base(152), 5);
        require(reuse.choice && reuse.choice->summary().reusable_prompt_tokens == 16,
                "reordered materialization result attached the second summary to another owner");
    }
}

// A FULL TARGET ARENA MUST DEGRADE, NOT THROW -- and this is the case that can fail.
//
// WHY IT EXISTS. The 2026-09-28 abort was `intern_target` throwing `std::length_error` when the pressure
// planner filled its 4096-target arena; the journal reported it as `WORKER RECOVER: pressure target arena
// is full [target-count]` and the client saw an HTTP 500 on top of a worker recovery, on EVERY tree
// (including the pre-change control), which made the e2e's phase 1 unreachable. The first version of the
// fix was believed tested by a run that PASSED; it passed without reaching the bound, which is not a test.
// Then a host test asserting the graceful path was written, its denominator showed it never reached the
// branch, and it was DELETED rather than kept green -- correctly, but that left the path uncovered, which
// a review then demonstrated with mutants: re-throwing instead of breaking, dropping the flag, dropping the
// counter increment and dropping the JSON key ALL survived the suite.
//
// WHAT IT PINS, in one run of the construction path (the one that fired in production):
//   * the request still produces a plan -- no throw, which is the whole point;
//   * `pressure_target_arena_truncations` INCREMENTS, so a capacity ceiling is counted and not silently
//     read as the search's own budget (`search_budget_exhaustions` must NOT move, which is the separation
//     the two counters exist for);
//   * and it prints both denominators, so "the branch was reached" is asserted rather than assumed.
void test_target_arena_bound_degrades_instead_of_throwing() {
    const auto run = [](bool refuse) {
        constexpr std::size_t owner_count = 7;
        FakeManager manager               = make_manager(1, owner_count + 1U);
        FakeProgram program;
        for (std::size_t index = 0; index < owner_count; ++index) {
            const std::uint32_t content = static_cast<std::uint32_t>(70U + index);
            const ActiveRequest active =
                start_active(manager, program, content, make_base(content), index + 1U);
            (void)finish_active(manager, program, active);
        }
        program.required_pressure_actions     = 3;
        program.private_pressure_alternatives = 4;
        program.pressure_assessment_delay_us  = 2'000;
        program.refuse_construction_targets   = refuse;
        // `Inspection` is not movable, so the facts are taken inside the try rather than the object kept.
        // THE RESERVE IS THE POINT: `observe_planner_diagnostics` -- the only place the planner's
        // diagnostics reach the counters -- runs during `reserve_materialization`, not during `inspect`.
        // A test that only inspects reports the counter as zero whatever the planner did.
        bool threw   = false;
        bool planned = false;
        try {
            auto inspection = manager.inspect(program, FakePreparedPrompt{90}, make_base(90), 20);
            planned         = inspection.choice.has_value();
            if (planned) {
                // NOT `abort_start`: an aborted reserve returns BEFORE `observe_planner_diagnostics`,
                // which is the only path from the planner's diagnostics to the counters -- so an
                // aborting arm would read zero however the planner behaved.
                (void)manager.reserve_materialization(program, std::move(*inspection.choice),
                                                      FakePreparedPrompt{90}, {});
            }
        } catch (const std::exception&) {
            threw = true;
        }
        RuntimeStats stats;
        manager.populate_runtime_stats(program, stats);
        return std::make_tuple(threw, planned, stats.pressure_target_arena_truncations,
                               stats.pressure_search_budget_exhaustions,
                               program.construction_target_calls);
    };

    auto [control_threw, control_planned, control_trunc, control_budget, control_calls] = run(false);
    auto [refused_threw, refused_planned, refused_trunc, refused_budget, refused_calls] = run(true);

    // THE DENOMINATORS FIRST: without these a scenario that never walks the path reports the same zeroes
    // as a graceful stop, and the assertions below would measure the scenario instead of the change.
    require(control_calls > 0 && refused_calls > 0,
            "THE CONSTRUCTION PATH WAS NEVER WALKED: `construction_target` was not called, so this test "
            "would report a graceful stop that never had a chance to be otherwise");
    require(control_trunc == 0,
            "the CONTROL arm truncated the arena: with the refusal knob off nothing should report a "
            "capacity ceiling, so this run cannot serve as a baseline");
    require(!control_threw && control_planned,
            "the control arm of the truncation test produced no admission plan at all");

    require(!refused_threw,
            "A FULL TARGET ARENA THREW: this is the 2026-09-28 HTTP 500 plus worker recovery -- the bound "
            "must truncate the search and let the request continue, not raise");
    if (!(refused_trunc > control_trunc)) {
        std::fprintf(stderr, "[diag] control_trunc=%llu refused_trunc=%llu control_calls=%u refused_calls=%u "
                             "refused_planned=%d control_planned=%d\n",
                     (unsigned long long)control_trunc, (unsigned long long)refused_trunc,
                     control_calls, refused_calls, (int)refused_planned, (int)control_planned);
    }
    require(refused_trunc > control_trunc,
            "the truncation was NOT counted: with the arena refusing new targets, "
            "pressure_target_arena_truncations must exceed the control's -- an arena ceiling that stops the "
            "search without a counter reads exactly like a healthy short search");
    // THE TWO COUNTERS PARTITION THE STOPS; THEY MUST NOT OVERLAP. Each arm makes ONE stop -- the
    // control's by budget, the refused arm's by arena -- so the sums must be equal. A construction stop
    // that set `budget_exhausted` as well (the first version did) would count the same event twice and
    // read 2 here, which is exactly the conflation these two counters exist to prevent: "the search spent
    // its own budget" is the DESIGNED stop, "no room for another target" is a CAPACITY CEILING, and a
    // ceiling reported as a budget stop is how the 4096-target wall filled unnoticed.
    require(refused_budget + refused_trunc == control_budget + control_trunc,
            "a single stop was counted TWICE -- once as an arena ceiling and once as a budget exhaustion. "
            "The two counters must partition the stops, not overlap");
}

void test_guided_pressure_reaches_deep_retention_before_maximal_fallback() {
    constexpr std::size_t owner_count = 7;
    FakeManager manager               = make_manager(1, owner_count + 1U);
    FakeProgram program;
    std::array<std::uint32_t, owner_count> owner_ids{};
    for (std::size_t index = 0; index < owner_count; ++index) {
        const std::uint32_t content = static_cast<std::uint32_t>(70U + index);
        const ActiveRequest active =
            start_active(manager, program, content, make_base(content), index + 1U);
        owner_ids[index] = active.sequence.id;
        (void)finish_active(manager, program, active);
    }

    program.required_pressure_actions     = 3;
    program.private_pressure_alternatives = 4;
    program.pressure_assessment_delay_us  = 2'000;
    auto inspection = manager.inspect(program, FakePreparedPrompt{90}, make_base(90), 20);
    require(inspection.choice.has_value(), "guided pressure search found no admission plan");

    program.abort_start = true;
    (void)manager.reserve_materialization(program, std::move(*inspection.choice),
                                          FakePreparedPrompt{90}, {});
    require(program.started_action_ids.size() == program.required_pressure_actions,
            "guided pressure search selected maximal release instead of a retention closure");
    for (const std::uint32_t owner_id : owner_ids) {
        require(std::find(program.started_action_ids.begin(), program.started_action_ids.end(),
                          2000U + owner_id) == program.started_action_ids.end(),
                "guided pressure search evicted a parked owner");
    }
    require(program.pressure_target_assessments <= 8,
            "guided pressure search returned to eager breadth-first assessment");
}

void test_combined_target_reprices_cancelled_pressure_copy() {
    FakeManager manager = make_manager(1, 3);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 51, make_base(51), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 52, make_base(52), 2);
    (void)finish_active(manager, program, second);

    program.required_pressure_actions             = 2;
    program.combined_target_cancels_pressure_copy = true;
    auto inspection = manager.inspect(program, FakePreparedPrompt{53}, make_base(53), 3);
    require(inspection.choice.has_value(),
            "non-monotonic complete target cost made the feasible combination unreachable");

    program.abort_start = true;
    (void)manager.reserve_materialization(program, std::move(*inspection.choice),
                                          FakePreparedPrompt{53}, {});
    std::sort(program.started_action_ids.begin(), program.started_action_ids.end());
    const std::vector<std::uint64_t> expected{
        1000U + first.sequence.id,
        1000U + second.sequence.id,
    };
    require(program.started_action_ids == expected,
            "planner accumulated parent transfer cost instead of repricing the complete target");
}

void test_in_progress_adoption_and_private_capture() {
    FakeManager manager = make_manager(1, 2);
    FakeProgram program;
    program.progress_in_progress_once = true;
    const ActiveRequest active        = start_active(manager, program, 12, make_base(12), 1);

    program.capture_assessment = FakeCaptureAssessment{
        .shortlist_key     = FakeShortlistKey{.digest = 12, .frontier = 24},
        .publishes_private = true,
    };
    program.capture_summary.endpoint = endpoint(12, 24);
    program.capture_summary.long_anchors.push_back(long_anchor(12, 16, 1));
    const auto reserved =
        manager.reserve_active_capture(program, active.lane, FakeCaptureOffer{.id = 1}, true, {});
    require(reserved == FakeManager::ActiveCaptureReserveResult::Reserved,
            "private capture was not reserved");
    auto progress = manager.progress_context_transaction(program, {});
    auto outcome  = std::get<FakeManager::ActiveCaptureOutcome>(std::move(progress));
    require(outcome.status == ContextTransactionStatus::Published &&
                !manager.context_transaction_kind(),
            "private capture was not adopted to a stable logical state");
    require(manager.lane_state(active.lane) == ninfer::runtime::LogicalLaneState::Active,
            "private capture disturbed active lane ownership");
    (void)finish_active(manager, program, active, 24);
}

void test_projected_nested_shared_candidates_use_marginal_value() {
    FakeManager manager = make_manager(1, 2, 2);
    FakeProgram program;
    FakeRequestBasePlan base = make_base(61);
    base.cache.opportunities = {
        FakeContextCache::Opportunity{
            .kind     = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
            .evidence = ninfer::SharedCandidateEvidence::EngineStructural,
            .frontier = 32,
        },
        FakeContextCache::Opportunity{
            .kind     = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
            .evidence = ninfer::SharedCandidateEvidence::EngineStructural,
            .frontier = 64,
        },
    };
    const ActiveRequest active = start_active(manager, program, 61, base, 1);
    require(program.selected_shared_capture_frontiers == std::vector<std::uint32_t>{64},
            "nested shared candidates were selected independently instead of by marginal value");
    (void)finish_active(manager, program, active);
}

void test_observed_shared_candidate_requires_independent_domains() {
    const auto observed_base = [](std::optional<FakeCacheSessionKey> session) {
        FakeRequestBasePlan base = make_base(71, session);
        base.cache.opportunities.push_back(FakeContextCache::Opportunity{
            .kind     = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
            .evidence = ninfer::SharedCandidateEvidence::EngineObserved,
            .frontier = 64,
        });
        return base;
    };

    {
        FakeManager manager = make_manager(1, 3, 1);
        FakeProgram program;
        const ActiveRequest first = start_active(manager, program, 71, observed_base({}), 1);
        require(program.selected_shared_capture_frontiers.empty(),
                "first stateless observation was treated as independent reuse");
        (void)finish_active(manager, program, first);
        const ActiveRequest second = start_active(manager, program, 71, observed_base({}), 2);
        require(program.selected_shared_capture_frontiers == std::vector<std::uint32_t>{64},
                "two stateless observations did not establish independent reuse demand");
        (void)finish_active(manager, program, second);
    }
    {
        FakeManager manager = make_manager(1, 3, 1);
        FakeProgram program;
        const FakeCacheSessionKey session{.value = 9};
        const ActiveRequest first = start_active(manager, program, 71, observed_base(session), 1);
        (void)finish_active(manager, program, first);
        const ActiveRequest second = start_active(manager, program, 71, observed_base(session), 2);
        require(program.selected_shared_capture_frontiers.empty(),
                "same-session replay was misclassified as shared fanout demand");
        (void)finish_active(manager, program, second);
    }
}

void test_repeated_private_reuse_selects_zero_prefill_shared_promotion() {
    const auto observed_base = [] {
        FakeRequestBasePlan base = make_base(81);
        base.cache.opportunities.push_back(FakeContextCache::Opportunity{
            .kind     = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
            .evidence = ninfer::SharedCandidateEvidence::EngineObserved,
            .frontier = 16,
        });
        return base;
    };

    FakeManager manager = make_manager(1, 3, 1);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 81, observed_base(), 1);
    require(program.selected_shared_capture_frontiers.empty(),
            "first observation selected an unsupported shared promotion");
    (void)finish_active(manager, program, first, 16);

    const ActiveRequest second = start_active(manager, program, 81, observed_base(), 2);
    require(program.selected_shared_capture_frontiers == std::vector<std::uint32_t>{16},
            "repeated private base was not selected for zero-prefill shared promotion");
    (void)finish_active(manager, program, second, 16);
}

void test_shared_fanout_keeps_owner_edges_live_across_summary_refresh() {
    FakeManager manager = make_manager(2, 3, 1);
    FakeProgram program;

    FakeRequestBasePlan seed_base = make_base(91);
    seed_base.cache.opportunities.push_back(FakeContextCache::Opportunity{
        .kind     = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
        .evidence = ninfer::SharedCandidateEvidence::ExplicitBoundary,
        .frontier = 64,
    });
    const ActiveRequest seed   = start_active(manager, program, 91, seed_base, 1);
    program.capture_assessment = FakeCaptureAssessment{
        .shortlist_key          = FakeShortlistKey{.digest = 91, .frontier = 64},
        .shared_evidence        = ninfer::SharedCandidateEvidence::ExplicitBoundary,
        .protected_rebuild_work = PrefillWork{.tokens = 64},
        .publishes_shared       = true,
        .physically_feasible    = true,
    };
    require(manager.reserve_active_capture(program, seed.lane, FakeCaptureOffer{.id = 31}, 0, {}) ==
                FakeManager::ActiveCaptureReserveResult::Reserved,
            "shared-fanout fixture could not publish its source");
    auto capture_progress = manager.progress_context_transaction(program, {});
    const auto capture = std::get<FakeManager::ActiveCaptureOutcome>(std::move(capture_progress));
    require(capture.status == ContextTransactionStatus::Published,
            "shared-fanout fixture did not publish its source");
    (void)finish_active(manager, program, seed);

    program.report_shared_source_summary                    = true;
    program.change_shared_source_residency_on_second_report = true;
    program.reported_shared_active_references               = 0;
    const ActiveRequest first  = start_active(manager, program, 91, make_base(91), 2);
    const ActiveRequest second = start_active(manager, program, 91, make_base(91), 3);

    RuntimeStats stats;
    manager.populate_runtime_stats(program, stats);
    require(stats.shared_active_references == 2,
            "two shared branches did not retain two logical owner edges");
    (void)finish_active(manager, program, first);
    manager.populate_runtime_stats(program, stats);
    require(stats.shared_active_references == 1,
            "first shared branch release invalidated the surviving owner edge");
    (void)finish_active(manager, program, second);
    manager.populate_runtime_stats(program, stats);
    require(stats.shared_active_references == 0,
            "shared fanout did not release both logical owner edges");
}

void test_shared_capture_combines_two_pressure_owners() {
    FakeManager manager = make_manager(1, 4, 1);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 41, make_base(41), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 42, make_base(42), 2);
    (void)finish_active(manager, program, second);

    FakeRequestBasePlan shared_request = make_base(43);
    shared_request.cache.opportunities.push_back(FakeContextCache::Opportunity{
        .kind     = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
        .evidence = ninfer::SharedCandidateEvidence::ExplicitBoundary,
        .frontier = 64,
    });
    const ActiveRequest active           = start_active(manager, program, 43, shared_request, 3);
    program.required_pressure_actions    = 2;
    program.pressure_action_immediate_ns = 0;
    program.capture_assessment           = FakeCaptureAssessment{
                  .shortlist_key          = FakeShortlistKey{.digest = 43, .frontier = 64},
                  .shared_evidence        = ninfer::SharedCandidateEvidence::ExplicitBoundary,
                  .protected_rebuild_work = PrefillWork{.tokens = 64},
                  .publishes_shared       = true,
                  .physically_feasible    = false,
    };

    const auto reserved =
        manager.reserve_active_capture(program, active.lane, FakeCaptureOffer{.id = 7}, 0, {});
    require(reserved == FakeManager::ActiveCaptureReserveResult::Reserved,
            "shared capture did not reserve a multi-owner pressure target");
    auto progress      = manager.progress_context_transaction(program, {});
    const auto outcome = std::get<FakeManager::ActiveCaptureOutcome>(std::move(progress));
    require(outcome.status == ContextTransactionStatus::Published &&
                program.started_action_ids.size() == 2,
            "shared capture did not publish the selected two-owner target");
    RuntimeStats stats;
    manager.populate_runtime_stats(program, stats);
    require(stats.shared_active_references == 1,
            "shared capture publication did not retain the active owner reference");
    program.required_pressure_actions = 0;
    (void)finish_active(manager, program, active);
}

void test_aborted_shared_capture_start_rolls_back_logical_claims() {
    FakeManager manager = make_manager(1, 4, 1);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 141, make_base(141), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 142, make_base(142), 2);
    (void)finish_active(manager, program, second);

    FakeRequestBasePlan request = make_base(143);
    request.cache.opportunities.push_back(FakeContextCache::Opportunity{
        .kind     = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
        .evidence = ninfer::SharedCandidateEvidence::ExplicitBoundary,
        .frontier = 64,
    });
    const ActiveRequest active           = start_active(manager, program, 143, request, 3);
    program.required_pressure_actions    = 2;
    program.pressure_action_immediate_ns = 0;
    program.capture_assessment           = FakeCaptureAssessment{
                  .shortlist_key          = FakeShortlistKey{.digest = 143, .frontier = 64},
                  .shared_evidence        = ninfer::SharedCandidateEvidence::ExplicitBoundary,
                  .protected_rebuild_work = PrefillWork{.tokens = 64},
                  .publishes_shared       = true,
                  .physically_feasible    = false,
    };
    program.abort_capture_start = true;

    const auto reserved =
        manager.reserve_active_capture(program, active.lane, FakeCaptureOffer{.id = 13}, 0, {});
    require(reserved == FakeManager::ActiveCaptureReserveResult::Skipped &&
                !manager.context_transaction_kind() && !program.has_context_transaction(),
            "aborted shared capture start retained transaction ownership");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Catalogued &&
                manager.catalog_state(1) == FakeManager::CatalogState::Catalogued,
            "aborted shared capture start leaked logical victim claims");

    program.abort_capture_start = false;
    const auto retried =
        manager.reserve_active_capture(program, active.lane, FakeCaptureOffer{.id = 14}, 0, {});
    require(retried == FakeManager::ActiveCaptureReserveResult::Reserved,
            "aborted shared capture start leaked the shared publication slot");
    auto progress      = manager.progress_context_transaction(program, {});
    const auto outcome = std::get<FakeManager::ActiveCaptureOutcome>(std::move(progress));
    require(outcome.status == ContextTransactionStatus::Published,
            "shared capture retry did not publish after rollback");

    program.required_pressure_actions = 0;
    (void)finish_active(manager, program, active);
}

void test_capture_result_is_validated_before_any_adoption() {
    FakeManager manager = make_manager(1, 4, 1);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 241, make_base(241), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 242, make_base(242), 2);
    (void)finish_active(manager, program, second);

    FakeRequestBasePlan request = make_base(243);
    request.cache.opportunities.push_back(FakeContextCache::Opportunity{
        .kind     = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
        .evidence = ninfer::SharedCandidateEvidence::ExplicitBoundary,
        .frontier = 64,
    });
    const ActiveRequest active           = start_active(manager, program, 243, request, 3);
    program.required_pressure_actions    = 2;
    program.pressure_action_immediate_ns = 0;
    program.capture_assessment           = FakeCaptureAssessment{
                  .shortlist_key          = FakeShortlistKey{.digest = 243, .frontier = 64},
                  .shared_evidence        = ninfer::SharedCandidateEvidence::ExplicitBoundary,
                  .protected_rebuild_work = PrefillWork{.tokens = 64},
                  .publishes_shared       = true,
                  .physically_feasible    = false,
    };
    program.malform_last_capture_private_victim = true;

    require(manager.reserve_active_capture(program, active.lane, FakeCaptureOffer{.id = 17}, 0,
                                           {}) == FakeManager::ActiveCaptureReserveResult::Reserved,
            "malformed capture fixture could not reserve pressure");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Claimed &&
                manager.catalog_state(1) == FakeManager::CatalogState::Claimed,
            "malformed capture fixture did not claim both owners");

    bool rejected = false;
    try {
        (void)manager.progress_context_transaction(program, {});
    } catch (const std::logic_error&) { rejected = true; }
    require(rejected, "malformed capture result was accepted");
    require(manager.catalog_state(0) == FakeManager::CatalogState::Claimed &&
                manager.catalog_state(1) == FakeManager::CatalogState::Claimed,
            "malformed capture result partially adopted an earlier victim");
}

void test_capture_result_is_adopted_by_owner_identity() {
    FakeManager manager = make_manager(1, 4, 1);
    FakeProgram program;
    const ActiveRequest first = start_active(manager, program, 251, make_base(251), 1);
    (void)finish_active(manager, program, first);
    const ActiveRequest second = start_active(manager, program, 252, make_base(252), 2);
    (void)finish_active(manager, program, second);

    FakeRequestBasePlan request = make_base(253);
    request.cache.opportunities.push_back(FakeContextCache::Opportunity{
        .kind     = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
        .evidence = ninfer::SharedCandidateEvidence::ExplicitBoundary,
        .frontier = 64,
    });
    const ActiveRequest active           = start_active(manager, program, 253, request, 3);
    program.required_pressure_actions    = 2;
    program.pressure_action_immediate_ns = 0;
    program.reverse_pressure_results     = true;
    program.capture_assessment           = FakeCaptureAssessment{
                  .shortlist_key          = FakeShortlistKey{.digest = 253, .frontier = 64},
                  .shared_evidence        = ninfer::SharedCandidateEvidence::ExplicitBoundary,
                  .protected_rebuild_work = PrefillWork{.tokens = 64},
                  .publishes_shared       = true,
                  .physically_feasible    = false,
    };

    require(manager.reserve_active_capture(program, active.lane, FakeCaptureOffer{.id = 27}, 0,
                                           {}) == FakeManager::ActiveCaptureReserveResult::Reserved,
            "reordered capture fixture could not reserve pressure");
    auto progress      = manager.progress_context_transaction(program, {});
    const auto outcome = std::get<FakeManager::ActiveCaptureOutcome>(std::move(progress));
    require(outcome.status == ContextTransactionStatus::Published &&
                manager.catalog_state(0) == FakeManager::CatalogState::Catalogued &&
                manager.catalog_state(1) == FakeManager::CatalogState::Catalogued,
            "reordered capture results changed victim availability");
    program.required_pressure_actions = 0;
    (void)finish_active(manager, program, active);

    {
        auto reuse = manager.inspect(program, FakePreparedPrompt{251}, make_base(251), 4);
        require(reuse.choice && reuse.choice->summary().reusable_prompt_tokens == 16,
                "reordered capture result attached the first summary to another owner");
    }
    {
        auto reuse = manager.inspect(program, FakePreparedPrompt{252}, make_base(252), 5);
        require(reuse.choice && reuse.choice->summary().reusable_prompt_tokens == 16,
                "reordered capture result attached the second summary to another owner");
    }
}

void test_terminal_fallback_releases_failed_retention() {
    FakeManager manager = make_manager(1, 1);
    FakeProgram program;
    const ActiveRequest active = start_active(manager, program, 4, make_base(4), 1);
    manager.mark_terminal_pending(active.lane);
    program.finish_fail_next      = true;
    const FakeFinishResult result = manager.finish(program, active.lane, active.sequence);
    require(result.status == ConsumeStatus::Consumed &&
                result.disposition == FinishDisposition::Released,
            "failed retention did not converge to a released terminal result");
    require(result.timings.value == 7 && result.speculative.value == 9,
            "terminal fallback lost abort accounting");
    require(program.abort_calls == 1 &&
                manager.lane_state(active.lane) == ninfer::runtime::LogicalLaneState::Free &&
                manager.catalog_state(0) == FakeManager::CatalogState::Vacant,
            "terminal fallback did not free every logical owner");
}

void test_terminal_settlement_waits_for_open_resource_transaction() {
    FakeManager manager = make_manager(2, 3);
    FakeProgram program;
    const ActiveRequest active = start_active(manager, program, 31, make_base(31), 1);

    auto inspection = manager.inspect(program, FakePreparedPrompt{32}, make_base(32), 2);
    require(inspection.choice.has_value(), "concurrent materialization choice was not produced");
    require(manager.reserve_materialization(program, std::move(*inspection.choice),
                                            FakePreparedPrompt{32}, {}) ==
                FakeManager::MaterializationReserveResult::Reserved,
            "concurrent materialization was not reserved");

    const auto capture =
        manager.reserve_active_capture(program, active.lane, FakeCaptureOffer{.id = 9}, true, {});
    require(capture == FakeManager::ActiveCaptureReserveResult::Skipped &&
                program.skipped_captures == 1 &&
                manager.context_transaction_kind() ==
                    ninfer::runtime::ContextTransactionKind::Materialization,
            "optional capture was not skipped behind the open materialization");

    manager.mark_terminal_pending(active.lane);
    bool rejected = false;
    try {
        (void)manager.finish(program, active.lane, active.sequence);
    } catch (const std::logic_error&) { rejected = true; }
    require(rejected && manager.lane_state(active.lane) ==
                            ninfer::runtime::LogicalLaneState::TerminalPending,
            "terminal settlement changed topology during an open resource transaction");
}

void test_commit_and_discard_terminal_states() {
    FakeManager manager = make_manager(2, 2);
    FakeProgram program;
    const ActiveRequest first  = start_active(manager, program, 1, make_base(1), 1);
    const ActiveRequest second = start_active(manager, program, 2, make_base(2), 2);
    const std::array<LaneId, 2> lanes{first.lane, second.lane};
    FakeCommitResult commit;
    commit.row_count           = 2;
    commit.rows[0].disposition = CommitDisposition::Active;
    commit.rows[1].disposition = CommitDisposition::Finishable;
    manager.apply_commit(lanes, commit);
    require(manager.lane_state(first.lane) == ninfer::runtime::LogicalLaneState::Active &&
                manager.lane_state(second.lane) ==
                    ninfer::runtime::LogicalLaneState::TerminalPending,
            "row-aligned commit did not establish terminal pending state");
    (void)manager.finish(program, second.lane, second.sequence);

    FakeDiscardResult discard{.status = ConsumeStatus::Consumed, .row_count = 1};
    const std::array<LaneId, 1> remaining{first.lane};
    manager.apply_discard(remaining, discard);
    require(manager.lane_state(first.lane) == ninfer::runtime::LogicalLaneState::Free,
            "discard did not release cancelled active membership");
}

void test_backfill_proof_and_stats_follow_program_revision() {
    FakeManager manager = make_manager();
    FakeProgram program;
    auto inspection = manager.inspect(program, FakePreparedPrompt{8}, make_base(8), 1);
    require(inspection.choice.has_value(), "backfill test did not produce a candidate plan");
    const std::array<FakeSequenceHandle, 0> borrowers{};
    auto proof =
        manager.prove_persistent_backfill(program, make_base(99), *inspection.choice, borrowers);
    require(proof && proof->resource_revision() == program.resource_revision(),
            "Program did not seal a current-revision persistent proof");
    program.invalidate_resources();
    proof =
        manager.prove_persistent_backfill(program, make_base(99), *inspection.choice, borrowers);
    require(!proof, "resource revision change did not invalidate persistent proof");

    program.usage = FakePhysicalUsage{
        .device_state_slots      = 3,
        .host_state_slots        = 2,
        .device_main_kv_pages    = 11,
        .device_backend_kv_pages = 5,
        .host_kv_bytes           = 4096,
    };
    RuntimeStats stats;
    manager.populate_runtime_stats(program, stats);
    require(stats.device_state_occupied_slots == 3 && stats.host_state_occupied_slots == 2 &&
                stats.device_main_kv_occupied_pages == 11 &&
                stats.device_backend_kv_occupied_pages == 5 && stats.host_kv_occupied_bytes == 4096,
            "runtime physical gauges did not come directly from Program");
}

void test_shortlist_collision_requires_program_exact_verification() {
    FakeManager manager = make_manager(1, 2);
    FakeProgram program;
    const ActiveRequest seed = start_active(manager, program, 55, make_base(123), 1);
    (void)finish_active(manager, program, seed);

    auto collision = manager.inspect(program, FakePreparedPrompt{99}, make_base(55), 2);
    require(collision.choice && collision.choice->summary().reusable_prompt_tokens == 0,
            "shortlist collision bypassed Program exact identity verification");
}

// Enumerates raw owner choices independently of the search frontier/generator and portfolio fold.
// Each owner can keep its cache, spill it (one relief unit), or drop it (two relief units).
void test_complete_search_against_small_exhaustive_oracle() {
    using Planner              = ninfer::runtime::MaterializationPlanner<FakeModelContract>;
    constexpr std::uint64_t ms = 1'000'000;
    const std::array<std::uint64_t, 3> rebuild{600 * ms, 200 * ms, 100 * ms};
    const std::array<std::uint64_t, 3> spill{40 * ms, 200 * ms, 10 * ms};
    const std::array<std::uint64_t, 3> drop{ms, 2 * ms, 3 * ms};
    for (unsigned weight_rotation = 0; weight_rotation < 3; ++weight_rotation) {
        for (unsigned required = 0; required <= 3; ++required) {
            for (bool full_catalog : {false, true}) {
                FakeProgram program;
                program.required_pressure_actions       = required;
                program.eviction_pressure_action_units  = 2;
                program.pressure_checkpoint_recovery_ns = 1'000 * ms;
                std::array<FakeContinuationHandle, 3> handles;
                std::array<const FakeContinuationHandle*, 3> owners;
                std::array<PlanningOwnerId, 3> ids;
                std::array<ninfer::runtime::MaterializationOwnerPolicy, 3> policies;
                std::array<ninfer::runtime::MaterializationCheckpointPolicy, 3> checkpoints;
                const std::array<unsigned, 3> weights{1, 4, 16};
                for (unsigned i = 0; i < 3; ++i) {
                    handles[i]     = FakeContinuationHandle{i + 1, 0};
                    owners[i]      = &handles[i];
                    ids[i]         = {.value = i};
                    policies[i]    = {.owner                    = ids[i],
                                      .private_retention_weight = weights[(i + weight_rotation) % 3]};
                    checkpoints[i] = {.owner       = ids[i],
                                      .checkpoint  = {.kind     = CheckpointKind::SessionEndpoint,
                                                      .frontier = 16,
                                                      .ordinal  = 0},
                                      .demand_mask = 1,
                                      .rebuild_ns  = rebuild[i]};
                    program.owner_decisions.push_back({i + 1,
                                                       {{.id = 1000 + i, .immediate_ns = spill[i]},
                                                        {.id                  = 2000 + i,
                                                         .immediate_ns        = drop[i],
                                                         .degradation_units   = 4,
                                                         .dropped_checkpoints = 1,
                                                         .evicts_continuation = true}}});
                }
                FakeAdmissionCandidate root, reuse;
                set_fake_machine_costs(root.identity.machine_work, 800 * ms, 800 * ms);
                set_fake_machine_costs(reuse.identity.machine_work, 100 * ms, 100 * ms);
                reuse.private_source_id                          = 1;
                reuse.value.reusable_prompt_tokens               = 48;
                reuse.identity.machine_work.reused_prompt_tokens = 48;
                for (auto* candidate : {&root, &reuse}) {
                    candidate->identity.physical_status =
                        required == 0 ? ninfer::runtime::MaterializationPhysicalStatus::Feasible
                                      : ninfer::runtime::MaterializationPhysicalStatus::Infeasible;
                    candidate->identity.expandable = required != 0;
                }
                const std::array candidates{
                    Planner::CandidateInput{.candidate = &root, .id = {.value = 0}},
                    Planner::CandidateInput{
                        .candidate = &reuse, .id = {.value = 1}, .stable_ordinal = 1}};
                const auto inputs = [&]() -> Planner::PressureInputs {
                    return {.private_owners    = owners,
                            .private_owner_ids = ids,
                            .owner_policy      = policies,
                            .checkpoint_policy = checkpoints};
                };
                const auto goal =
                    [&](PlanningCandidateId candidate, PrivateSourceMode,
                        std::span<const ninfer::runtime::PressureOwnerOutcome> outcomes)
                    -> std::optional<Planner::LogicalGoal> {
                    if (candidate.value == 1 || !full_catalog ||
                        std::any_of(outcomes.begin(), outcomes.end(), [](auto outcome) {
                            return outcome.disposition == VictimDisposition::Evicted;
                        })) {
                        return Planner::LogicalGoal{.publication_slot = 0};
                    }
                    return std::nullopt;
                };
                std::uint64_t oracle           = UINT64_MAX;
                // The other half of the ranking: the oracle now minimises (restorable evictions, cost), the
                // order `FoldedCost::key()` uses, so the pair needs a second accumulator.
                std::uint32_t oracle_evictions = UINT32_MAX;
                for (unsigned source = 0; source < 2; ++source) {
                    for (unsigned raw = 0; raw < 27; ++raw) {
                        std::uint32_t evictions = 0;
                        unsigned digits = raw, relief = 0;
                        bool frees_slot = false, protects_source = true;
                        std::uint64_t cost                    = (source ? 100 : 800) * ms;
                        std::uint64_t remaining_public_saving = 0;
                        for (unsigned owner = 0; owner < 3; ++owner) {
                            unsigned choice = digits % 3;
                            digits /= 3;
                            if (source == 1 && owner == 0 && choice != 0) {
                                protects_source = false;
                            }
                            relief += choice;
                            if (choice == 2) {
                                frees_slot = true;
                                // THE POLICY THIS ORACLE ENCODES, and it changed under it (2026-09-27). The
                                // operator's ruling is "evicting a victim that holds a restorable checkpoint
                                // while the host has room is a defect", and `7746a98b` made the planner rank
                                // RESTORABLE EVICTIONS ahead of cost. This oracle still chose the cheapest
                                // plan, so it began failing on a configuration where the cheapest plan
                                // evicts (912 ms, 1 eviction) while the best plan under the ruling does not
                                // (1050 ms, 0). The planner picked 1050 -- the ruling, applied.
                                //
                                // So the oracle is updated to the policy rather than the case bent to go
                                // green: in THIS test every chosen owner reports a restorable checkpoint, so
                                // the count is simply the number of evicted owners.
                                ++evictions;
                                cost += drop[owner] +
                                        rebuild[owner] * weights[(owner + weight_rotation) % 3];
                            } else {
                                remaining_public_saving =
                                    std::max(remaining_public_saving, rebuild[owner]);
                                if (choice == 1) { cost += spill[owner]; }
                            }
                        }
                        if (!protects_source || relief < required ||
                            (full_catalog && source == 0 && !frees_slot)) {
                            continue;
                        }
                        cost += rebuild[0] - remaining_public_saving;
                        // LEXICOGRAPHIC, the way `FoldedCost::key()` now is: fewer restorable evictions wins
                        // outright, and cost decides among equals.
                        if (evictions < oracle_evictions ||
                            (evictions == oracle_evictions && cost < oracle)) {
                            oracle_evictions = evictions;
                            oracle           = cost;
                        }
                    }
                }
                Planner planner;
                auto allowance     = ninfer::runtime::PlanningAllowance::boundary(0);
                allowance.limit_ns = 5 * ms;
                auto result =
                    planner.plan(program, FakePreparedPrompt{}, test_cost_model(), candidates, 0,
                                 inputs, goal, Planner::Clock::now(), allowance);
                require(result && result->plan,
                        "exhaustive-oracle problem lost its feasible fallback");
                if (result->diagnostics.predicted_total_ns != oracle) {
                    std::cerr << "oracle rotation=" << weight_rotation << " relief=" << required
                              << " full=" << full_catalog << " expected=" << oracle
                              << " selected=" << result->diagnostics.predicted_total_ns << '\n';
                }
                require(result->diagnostics.predicted_total_ns == oracle,
                        "complete search missed the independent small-problem optimum");
                require(
                    std::none_of(result->plan->private_owner_ids.begin(),
                                 result->plan->private_owner_ids.end(),
                                 [&](auto id) { return result->candidate.value == 1 && id == 1; }),
                    "construction included the selected source as a victim");
            }
        }
    }
}

void test_publication_only_pressure_constructs_adoptable_target() {
    constexpr unsigned owners = 7;
    FakeManager manager       = make_manager(1, owners);
    FakeProgram program;
    for (unsigned i = 0; i < owners; ++i) {
        const auto active = start_active(manager, program, 500 + i, make_base(500 + i), i + 1);
        (void)finish_active(manager, program, active);
    }
    require(program.required_pressure_actions == 0, "publication fixture has physical pressure");
    auto allowance     = ninfer::runtime::PlanningAllowance::boundary(0);
    allowance.limit_ns = 5'000'000;
    auto result = manager.inspect(program, FakePreparedPrompt{600}, make_base(600), 20, allowance);
    require(result.choice.has_value(),
            "publication-only pressure did not produce an adoptable target");
    program.abort_start = true;
    (void)manager.reserve_materialization(program, std::move(*result.choice),
                                          FakePreparedPrompt{600}, {});
    require(program.started_action_ids.size() == 1 && program.started_action_ids.front() >= 2000,
            "publication-only closure did not release exactly one private slot");
}

// Value-aware demote-vs-evict selection: when two private victims have DIFFERENT re-prefill
// cost (endpoint rebuild_work) and device pressure forces one to demote-to-host and one to
// evict-and-drop, the planner must demote the HIGHER-value victim (keep its restorable
// checkpoint) and evict the cheaper one. The value weight is a property of the shared
// materialization objective (the portfolio-value fold priced from checkpoint rebuild_ns), so
// this drives the real MaterializationPlanner directly with two owners of distinct rebuild_ns
// and gates feasibility through logical_goal to model the host-arena constraint.
void test_value_aware_pressure_demotes_high_value_victim_over_eviction() {
    using Planner = ninfer::runtime::MaterializationPlanner<FakeModelContract>;
    constexpr std::uint64_t ms           = 1'000'000;
    constexpr std::uint32_t weight       = 4;
    constexpr std::uint64_t rebuild_high = 400 * ms;
    constexpr std::uint64_t rebuild_low  = 40 * ms;
    // Above both rebuild costs, so an evicted victim loses its full rebuild value in the
    // portfolio fold while a retained/demoted victim (zero restore cost) loses nothing.
    constexpr std::uint64_t recovery_cost = 1000 * ms;

    // Owner 1 carries the high re-prefill value; owner 2 the low value.
    const std::array<FakeContinuationHandle, 2> handles{
        FakeContinuationHandle{1, 0},
        FakeContinuationHandle{2, 0},
    };
    const std::array<const FakeContinuationHandle*, 2> owners{&handles[0], &handles[1]};
    const std::array<PlanningOwnerId, 2> ids{
        PlanningOwnerId{.value = 0},
        PlanningOwnerId{.value = 1},
    };
    const std::array<ninfer::runtime::MaterializationOwnerPolicy, 2> policies{
        ninfer::runtime::MaterializationOwnerPolicy{.owner                    = ids[0],
                                                    .private_retention_weight = weight},
        ninfer::runtime::MaterializationOwnerPolicy{.owner                    = ids[1],
                                                    .private_retention_weight = weight},
    };
    const std::array<ninfer::runtime::MaterializationCheckpointPolicy, 2> checkpoints{
        ninfer::runtime::MaterializationCheckpointPolicy{
            .owner       = ids[0],
            .checkpoint  = CheckpointRef{.kind     = CheckpointKind::SessionEndpoint,
                                         .frontier = 16,
                                         .ordinal  = 0},
            .demand_mask = 1,
            .rebuild_ns  = rebuild_high},
        ninfer::runtime::MaterializationCheckpointPolicy{
            .owner       = ids[1],
            .checkpoint  = CheckpointRef{.kind     = CheckpointKind::SessionEndpoint,
                                         .frontier = 16,
                                         .ordinal  = 0},
            .demand_mask = 1,
            .rebuild_ns  = rebuild_low},
    };
    // Each owner can demote-to-host (preserve, one relief unit) or evict-and-drop (one relief
    // unit). Both owners are touched under primary pressure; the host constraint decides the mix.
    const std::vector<std::pair<std::uint32_t, std::vector<FakeTargetDecision>>> owner_decisions{
        {1,
         {{.id                  = 1000U + 1, .immediate_ns = 0, .degradation_units = 1},
          {.id                  = 2000U + 1,
           .immediate_ns        = 0,
           .degradation_units   = 4,
           .dropped_checkpoints = 1,
           .evicts_continuation = true}}},
        {2,
         {{.id                  = 1000U + 2, .immediate_ns = 0, .degradation_units = 1},
          {.id                  = 2000U + 2,
           .immediate_ns        = 0,
           .degradation_units   = 4,
           .dropped_checkpoints = 1,
           .evicts_continuation = true}}},
    };
    const auto inputs = [&]() -> Planner::PressureInputs {
        return Planner::PressureInputs{
            .private_owners    = owners,
            .private_owner_ids = ids,
            .shared_owners     = {},
            .shared_owner_ids  = {},
            .owner_policy      = policies,
            .checkpoint_policy = checkpoints,
        };
    };

    // Primary: host room holds exactly one demote, so one victim must evict. The value-aware
    // objective must demote the high-value owner and evict the low-value owner.
    {
        FakeProgram program;
        program.required_pressure_actions        = 2;
        program.eviction_pressure_action_units   = 1;
        program.pressure_action_immediate_ns     = 0;
        program.pressure_checkpoint_recovery_ns  = recovery_cost;
        program.owner_decisions                  = owner_decisions;

        FakeAdmissionCandidate root;
        set_fake_machine_costs(root.identity.machine_work, 100 * ms, 100 * ms);
        root.identity.physical_status            =
            ninfer::runtime::MaterializationPhysicalStatus::Infeasible;
        root.identity.expandable                 = true;
        root.identity.assessment_digest          = 7;
        const std::array<Planner::CandidateInput, 1> candidates{
            Planner::CandidateInput{.candidate        = &root,
                                    .id               = PlanningCandidateId{.value = 0},
                                    .stable_ordinal   = 0,
                                    .current_session_binding = false},
        };
        const auto goal = [](PlanningCandidateId, PrivateSourceMode,
                             std::span<const ninfer::runtime::PressureOwnerOutcome> outcomes)
            -> std::optional<Planner::LogicalGoal> {
            std::uint32_t evicted = 0;
            for (const auto& outcome : outcomes) {
                if (outcome.disposition == VictimDisposition::Evicted) { ++evicted; }
            }
            if (evicted == 0) { return std::nullopt; }  // host can't hold every demote
            return Planner::LogicalGoal{.publication_slot = 0};
        };
        Planner planner;
        auto allowance                      = ninfer::runtime::PlanningAllowance::boundary(0);
        allowance.limit_ns                  = 100 * ms;
        const auto result                   =
            planner.plan(program, FakePreparedPrompt{}, test_cost_model(), candidates, 0, inputs,
                         goal, Planner::Clock::now(), allowance);
        require(result && result->plan,
                "value-aware pressure search found no demote/evict closure");
        const auto& plan = *result->plan;
        require(plan.private_owner_ids.size() == 2 && plan.private_actions.size() == 2,
                "value-aware pressure closure did not act on both victims");
        std::uint64_t action_for_high = 0, action_for_low = 0;
        for (std::size_t index = 0; index < plan.private_owner_ids.size(); ++index) {
            if (plan.private_owner_ids[index] == 1) { action_for_high = plan.private_actions[index].id; }
            if (plan.private_owner_ids[index] == 2) { action_for_low = plan.private_actions[index].id; }
        }
        require(action_for_high == 1000U + 1,
                "planner evicted (or failed to demote) the HIGH-value victim instead of demoting it");
        require(action_for_low == 2000U + 2,
                "planner demoted the LOW-value victim and evicted a higher-value one");
        // The evicted victim is the low-value owner, so the value-aware cost is the candidate
        // immediate plus rebuild_low * retention_weight; the retained high-value owner keeps the
        // public portfolio value, so it adds no future loss.
        const std::uint64_t expected = 100 * ms + static_cast<std::uint64_t>(weight) * rebuild_low;
        require(result->diagnostics.predicted_total_ns == expected,
                "value-aware cost model mis-priced the demote-high/evict-low closure");
    }

    // Fallback: host is full (no demote successor), so the victim that must be sacrificed evicts.
    // With relief = 1 the cheaper (low-value) victim evicts, preserving the value-aware outcome.
    {
        FakeProgram program;
        program.required_pressure_actions        = 1;
        program.eviction_pressure_action_units   = 1;
        program.pressure_action_immediate_ns     = 0;
        program.pressure_checkpoint_recovery_ns  = recovery_cost;
        program.owner_decisions                  = owner_decisions;

        FakeAdmissionCandidate root;
        set_fake_machine_costs(root.identity.machine_work, 100 * ms, 100 * ms);
        root.identity.physical_status            =
            ninfer::runtime::MaterializationPhysicalStatus::Infeasible;
        root.identity.expandable                 = true;
        root.identity.assessment_digest          = 7;
        const std::array<Planner::CandidateInput, 1> candidates{
            Planner::CandidateInput{.candidate        = &root,
                                    .id               = PlanningCandidateId{.value = 0},
                                    .stable_ordinal   = 0,
                                    .current_session_binding = false},
        };
        const auto goal = [](PlanningCandidateId, PrivateSourceMode,
                             std::span<const ninfer::runtime::PressureOwnerOutcome> outcomes)
            -> std::optional<Planner::LogicalGoal> {
            std::uint32_t evicted = 0;
            for (const auto& outcome : outcomes) {
                if (outcome.disposition == VictimDisposition::Evicted) { ++evicted; }
            }
            if (evicted == 0) { return std::nullopt; }  // host full: a demote is impossible
            return Planner::LogicalGoal{.publication_slot = 0};
        };
        Planner planner;
        auto allowance                      = ninfer::runtime::PlanningAllowance::boundary(0);
        allowance.limit_ns                  = 100 * ms;
        const auto result                   =
            planner.plan(program, FakePreparedPrompt{}, test_cost_model(), candidates, 0, inputs,
                         goal, Planner::Clock::now(), allowance);
        require(result && result->plan, "full-host pressure search found no evicting closure");
        const auto& plan = *result->plan;
        require(plan.private_owner_ids.size() == 1,
                "full-host closure evicted more than the one required victim");
        require(plan.private_owner_ids[0] == 2 && plan.private_actions[0].id == 2000U + 2,
                "full-host closure did not evict the cheaper (low-value) victim");
        // Same value-aware pricing as the primary closure: the sacrificed victim is the
        // low-value owner, so the cost is the candidate immediate plus rebuild_low * weight.
        const std::uint64_t expected = 100 * ms + static_cast<std::uint64_t>(weight) * rebuild_low;
        require(result->diagnostics.predicted_total_ns == expected,
                "full-host closure was not priced as a low-value eviction");
    }
}

// Deterministic check of the REAL value-aware victim ranking that populate_options charges
// to the evict cost: the highest re-prefill-cost private victim gets the highest
// value_weight (so the planner demotes it to host), the cheapest gets 0 (so it is evicted),
// and shared victims are always weight 0. This drives pressure_value_ranking.h directly —
// the exact function the production planner calls — with no GPU/program, so the
// demote-high/evict-low choice is reproducible on demand (unlike the e2e pressure search,
// a rare timing race).
void test_value_weights_rank_private_victims_by_rebuild_cost() {
    using ninfer::models::qwen3_5::detail::value_weights_for_victims;

    // Costs: A=high, B=low, C=mid, D=shared. Expected weights: A=3, B=0, C=1, D=0.
    {
        const std::array<std::uint8_t, 4> is_shared  = {0, 0, 0, 1};
        const std::array<std::uint64_t, 4> cost      = {9000, 100, 5000, 0};
        const auto weights                           = value_weights_for_victims(is_shared, cost);
        require(weights.size() == 4, "weight vector size mismatch");
        // 3 non-shared victims -> ranks are 0..2; the highest cost takes the top rank (2).
        require(weights[0] == 2, "high-cost private victim did not get the top rank");
        require(weights[1] == 0, "lowest-cost private victim did not get rank 0");
        require(weights[2] == 1, "mid-cost private victim did not get rank 1");
        require(weights[3] == 0, "shared victim must stay weight 0");
    }

    // All shared -> all weights 0 (nothing to rank).
    {
        const std::array<std::uint8_t, 2> is_shared  = {1, 1};
        const std::array<std::uint64_t, 2> cost      = {123, 456};
        const auto weights                           = value_weights_for_victims(is_shared, cost);
        require(weights[0] == 0 && weights[1] == 0, "all-shared victims must all be weight 0");
    }

    // Ties are deterministic (the victim index breaks them): equal costs keep ascending order.
    {
        const std::array<std::uint8_t, 3> is_shared  = {0, 0, 0};
        const std::array<std::uint64_t, 3> cost      = {7, 7, 7};
        const auto weights                           = value_weights_for_victims(is_shared, cost);
        require(weights[0] == 0 && weights[1] == 1 && weights[2] == 2,
                "equal-cost victims must keep deterministic ascending order");
    }

    // Empty: no victims, no weights.
    {
        const std::array<std::uint8_t, 0> is_shared  = {};
        const std::array<std::uint64_t, 0> cost      = {};
        const auto weights                           = value_weights_for_victims(is_shared, cost);
        require(weights.empty(), "no victims -> no weights");
    }
}


} // namespace

int main() {
    run_test("independent complete-target oracle",
             test_complete_search_against_small_exhaustive_oracle);
    run_test("unplannable request with no active lane is permanently infeasible",
             test_unplannable_request_with_no_active_lane_is_permanently_infeasible);
    run_test("publication-only construction",
             test_publication_only_pressure_constructs_adoptable_target);
    run_test("value-aware demotes high-value victim over eviction",
             test_value_aware_pressure_demotes_high_value_victim_over_eviction);
    run_test("value-aware ranking of private victims by rebuild cost",
             test_value_weights_rank_private_victims_by_rebuild_cost);
    run_test("private checkpoint identity loss",
             test_private_portfolio_loss_keeps_checkpoint_identity_fixed);
    run_test("portfolio demand and owner aggregation", test_portfolio_demand_and_owner_aggregation);
    run_test("shared capture private transition loss",
             test_shared_capture_subtracts_private_transition_loss);
    run_test("shared capture committed target budget",
             test_shared_capture_budget_bounds_committed_canonical_targets);
    run_test("equal lower-bound tie-break",
             test_equal_lower_bound_does_not_short_circuit_tie_break);
    run_test("machine cost is selection-only",
             test_machine_cost_changes_selection_without_changing_physical_assessment);
    run_test("candidate-stratified reuse closure",
             test_candidate_search_prefers_deep_reuse_without_eviction);
    run_test("feasible identity pressure improvement",
             test_feasible_identity_expands_when_pressure_can_remove_copy);
    run_test("dominating identity fast path",
             test_dominating_identity_does_not_build_pressure_graph);
    run_test("root lifecycle and prefix reuse", test_root_lifecycle_and_prefix_reuse);
    run_test("session erase counters are wired", test_session_erase_counters_are_wired);
    run_test("session erase counts an eviction as eviction",
             test_session_erase_counts_an_eviction_as_eviction);
    run_test("host state census counts unanchored checkpoints",
             test_host_state_census_counts_unanchored_checkpoints);
    run_test("session endpoint reason is per frontier", test_session_endpoint_reason_is_per_frontier);
    run_test("prefix split diagnostics follow the catalog",
             test_prefix_split_diagnostics_follow_catalog);
    run_test("stale revision is retryable", test_stale_revision_is_retryable);
    run_test("materialization abort preserves source", test_materialization_abort_preserves_source);
    run_test("committed victim survives abort", test_committed_victim_survives_transaction_abort);
    run_test("uncommitted pressure acknowledgement",
             test_uncommitted_pressure_acknowledgement_is_not_degradation);
    run_test("aborted source is not a hit",
             test_aborted_source_selection_does_not_create_hit_history);
    run_test("retained source protection", test_retained_source_is_protected_until_terminal);
    run_test("session publication order", test_session_publication_order_controls_tied_source);
    run_test("canonical pressure", test_canonical_pressure_starts_with_disposable_owner);
    run_test("all preserving pressure alternatives",
             test_pressure_tries_every_preserving_alternative_before_eviction);
    run_test("cumulative owner target",
             test_cumulative_owner_target_closes_pressure_without_eviction);
    run_test("joint two-owner pressure", test_two_owners_jointly_close_pressure);
    run_test("validate complete materialization result before adoption",
             test_materialization_result_is_validated_before_any_adoption);
    run_test("materialization result checkpoint identity",
             test_materialization_result_binds_exact_checkpoint_identity);
    run_test("materialization result owner identity",
             test_materialization_result_is_adopted_by_owner_identity);
    run_test("guided deep retention",
             test_guided_pressure_reaches_deep_retention_before_maximal_fallback);
    run_test("target arena degrades", test_target_arena_bound_degrades_instead_of_throwing);
    run_test("combined target exact repricing",
             test_combined_target_reprices_cancelled_pressure_copy);
    run_test("in-progress and capture", test_in_progress_adoption_and_private_capture);
    run_test("projected shared marginal value",
             test_projected_nested_shared_candidates_use_marginal_value);
    run_test("observed shared independent domains",
             test_observed_shared_candidate_requires_independent_domains);
    run_test("zero-prefill private promotion",
             test_repeated_private_reuse_selects_zero_prefill_shared_promotion);
    run_test("shared fanout owner edges",
             test_shared_fanout_keeps_owner_edges_live_across_summary_refresh);
    run_test("shared capture multi-owner pressure",
             test_shared_capture_combines_two_pressure_owners);
    run_test("aborted shared capture logical rollback",
             test_aborted_shared_capture_start_rolls_back_logical_claims);
    run_test("validate complete capture result before adoption",
             test_capture_result_is_validated_before_any_adoption);
    run_test("capture result owner identity", test_capture_result_is_adopted_by_owner_identity);
    run_test("terminal fallback", test_terminal_fallback_releases_failed_retention);
    run_test("terminal waits for resource transaction",
             test_terminal_settlement_waits_for_open_resource_transaction);
    run_test("commit and discard", test_commit_and_discard_terminal_states);
    run_test("backfill proof and stats", test_backfill_proof_and_stats_follow_program_revision);
    run_test("shortlist exact verification",
             test_shortlist_collision_requires_program_exact_verification);
    if (failures != 0) {
        std::cout << tests_run << " run, " << failures << " failed\n";
        return 1;
    }
    std::cout << "ok (" << tests_run << " cases)\n";
    return 0;
}
