#include "models/qwen3_5/program/program_impl.h"
#include "core/diagnostics.h"
#include "models/qwen3_5/program/context_work.h"
#include "models/qwen3_5/program/planning/pressure_planner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace ninfer::models::qwen3_5::detail {

bool ProgramImpl::shared_capture_matches(const CaptureOffer& offer,
                                         const SharedPrefixHandle& shared) const {
    if (!valid_capture_offer(offer) || !valid_shared_prefix(shared)) { return false; }
    const std::uint32_t lane               = ContractAccess::lane(offer).value;
    const RequestControl::Prefill& prefill = *requests[lane].prefill;
    const CaptureGroup& group              = prefill.capture_groups[prefill.next_capture];
    const SharedPrefixState& candidate     = shared_prefix_states[ContractAccess::index(shared)];
    return group.shared && group.identity && candidate.identity &&
           group.frontier == candidate.frontier &&
           group.identity->shortlist_key == candidate.identity->shortlist_key &&
           group.identity->prefix_equals(*candidate.identity);
}

CaptureAssessment
ProgramImpl::inspect_capture(const CaptureOffer& offer, const SharedPrefixHandle* exact_shared,
                             const SharedPrefixHandle* replacement,
                             std::optional<runtime::CheckpointRef> private_replacement,
                             bool permit_shared_publication, const char* site) const {
    if (!valid_capture_offer(offer)) { throw std::logic_error("capture offer is stale"); }
    if (exact_shared != nullptr && replacement != nullptr) {
        throw std::invalid_argument("capture cannot deduplicate and replace simultaneously");
    }
    if (exact_shared != nullptr && !shared_capture_matches(offer, *exact_shared)) {
        throw std::logic_error("capture dedup source is not exact");
    }
    if (replacement != nullptr) {
        if (!valid_shared_prefix(*replacement)) {
            throw std::logic_error("capture replacement capability is stale");
        }
        const SharedPrefixState& victim = shared_prefix_states[ContractAccess::index(*replacement)];
        if (victim.active_references != 0) {
            throw std::logic_error("active-referenced shared prefix is not replaceable");
        }
    }
    const std::uint32_t lane               = ContractAccess::lane(offer).value;
    const RequestControl::Prefill& prefill = *requests[lane].prefill;
    const CaptureGroup& group              = prefill.capture_groups[prefill.next_capture];
    if (!group.identity) { throw std::logic_error("capture identity backing is missing"); }
    const bool publish_private =
        group.rewrite.has_value() ||
        (group.long_anchor && context_cache.max_long_anchors_per_continuation.value_or(0) != 0);
    const bool publish_shared = group.shared && permit_shared_publication &&
                                exact_shared == nullptr && shared_prefix_capacity != 0;

    CaptureAssessment assessment;
    assessment.shortlist_key   = group.identity->shortlist_key;
    assessment.shared_evidence = group.shared_evidence;
    assessment.protected_rebuild_work =
        validated_rebuild_work(group.identity->rebuild_work, group.frontier);
    assessment.frontier          = group.frontier;
    assessment.publishes_private = publish_private;
    assessment.publishes_shared  = publish_shared;
    if (!publish_private && !publish_shared) {
        if (private_replacement) {
            throw std::invalid_argument("empty capture has a private replacement");
        }
        assessment.physically_feasible = true;
        return assessment;
    }

    const SequenceState& sequence = active_sequence(lane);
    if (!sequence.kv) { throw std::logic_error("capture source has no KV bundle"); }
    const std::size_t anchor_limit = context_cache.max_long_anchors_per_continuation.value_or(0);
    const bool anchor_replacement_required =
        group.long_anchor && anchor_limit != 0 && sequence.long_anchors.size() == anchor_limit;
    const LongAnchorCheckpoint* selected_anchor_replacement = nullptr;
    if (anchor_replacement_required) {
        assessment.private_replacement_candidates.reserve(sequence.long_anchors.size());
        for (const LongAnchorCheckpoint& anchor : sequence.long_anchors) {
            assessment.private_replacement_candidates.push_back(runtime::CheckpointRef{
                .kind     = runtime::CheckpointKind::LongAnchor,
                .frontier = anchor.frontier,
                .ordinal  = anchor.ordinal,
            });
            if (private_replacement &&
                *private_replacement == assessment.private_replacement_candidates.back()) {
                selected_anchor_replacement = &anchor;
            }
        }
        if (private_replacement && selected_anchor_replacement == nullptr) {
            throw std::logic_error("private capture replacement is stale");
        }
        if (!private_replacement) { return assessment; }
    } else if (private_replacement) {
        throw std::invalid_argument("capture has no replaceable private anchor");
    }

    const bool replaces_rewrite =
        group.rewrite && sequence.rewrite_state && sequence.rewrite_checkpoint.valid;
    assessment.recycles_private_state =
        replaces_rewrite && *sequence.rewrite_state != sequence.state.write &&
        state_store->can_recycle_checkpoint_destination(*sequence.rewrite_state);
    // Per-capture probe (NINFER_CAPTURE_PROBE): the five conditions above, read where they are computed
    // rather than only at the one transfer-submitted site the abort probe reaches. Designing the
    // scenario for #11(a) needs to know WHICH condition fails on each turn, and the previous design
    // pass had only a single-hit reading to go on. Cheap, env-gated, and it needs no injector.
    if (std::getenv("NINFER_CAPTURE_PROBE") != nullptr) {
        const bool state_live =
            sequence.rewrite_state.has_value() && state_store->valid(*sequence.rewrite_state);
        // Reachability assert for #11(a), the plan's next action: if `recycles_private_state` can never
        // be true, this fires zero times across every real-engine scenario, and the recycling branch is
        // dead code rather than a latent hazard. Deliberately an abort, not a print: a print can be read
        // as "did not happen" when the probe was skipped, which is the mistake that produced two wrong
        // conclusions in this file's neighbourhood today.
        if (assessment.recycles_private_state) {
            std::fprintf(stderr, "[capture] ASSERT recycles_private_state is REACHABLE lane=%u\n",
                         lane);
            std::fflush(stderr);
            std::abort();
        }
        std::fprintf(stderr,
                     "[capture] assess site=%s lane=%u placement=%d dev_occ=%u dev_cap=%u host_images=%d "
                     "shared_group=%d publish_shared=%d "
                     "rewrite_group=%d rewrite_state_live=%d "
                     "checkpoint_valid=%d slot_differs=%d can_recycle=%d replaces_rewrite=%d "
                     "recycles=%d frontier=%u base=%u refs=%d\n",
                     site != nullptr ? site : "?", lane, static_cast<int>(assessment.state_placement),
                     state_store->device_occupied(),
                     state_store->device_capacity(),
                     static_cast<int>(host_state_images != nullptr),
                     static_cast<int>(group.shared), static_cast<int>(publish_shared),
                     static_cast<int>(group.rewrite.has_value()),
                     static_cast<int>(state_live),
                     static_cast<int>(sequence.rewrite_checkpoint.valid),
                     static_cast<int>(state_live &&
                                      *sequence.rewrite_state != sequence.state.write),
                     static_cast<int>(state_live &&
                                      state_store->can_recycle_checkpoint_destination(
                                          *sequence.rewrite_state)),
                     static_cast<int>(replaces_rewrite),
                     static_cast<int>(assessment.recycles_private_state), group.frontier,
                     sequence.rewrite_checkpoint.frontier,
                     static_cast<int>(state_live
                                          ? state_store->checkpoint_references(
                                                *sequence.rewrite_state)
                                          : -1));
        std::fflush(stderr);
    }
    detail::PhysicalResources added;
    detail::PhysicalResources active_removed;
    std::optional<KVActiveSnapshotShape> text_snapshot_shape;
    std::optional<KVActiveSnapshotShape> backend_snapshot_shape;
    if (publish_shared) {
        if (!sequence.kv) { throw std::logic_error("capture source has no KV bundle"); }
        text_snapshot_shape =
            text_kv_addresses->active_snapshot_shape(sequence.kv->text, sequence.text_kv_valid);
        active_removed.device.main_kv_pages = text_snapshot_shape->unique_full_pages;
        added.device.main_kv_pages          = text_snapshot_shape->copied_pages();

        if (sequence.kv->backend) {
            const std::uint32_t backend_frontier = backend_kv_valid(sequence);
            backend_snapshot_shape               = backend_kv_addresses->active_snapshot_shape(
                *sequence.kv->backend, backend_frontier);
            active_removed.device.backend_kv_pages = backend_snapshot_shape->unique_full_pages;
            added.device.backend_kv_pages          = backend_snapshot_shape->copied_pages();
        }
    }

    detail::PhysicalResources replaced_private;
    if (publish_private) {
        struct DroppedReference {
            StateImageHandle state;
            std::uint32_t count = 0;
        };

        std::array<DroppedReference, 2> drops{};
        std::size_t drop_count = 0;
        const auto add_drop    = [&](StateImageHandle state) {
            for (std::size_t index = 0; index < drop_count; ++index) {
                if (drops[index].state == state) {
                    ++drops[index].count;
                    return;
                }
            }
            drops[drop_count++] = DroppedReference{.state = state, .count = 1};
        };
        if (group.rewrite && sequence.rewrite_state) { add_drop(*sequence.rewrite_state); }
        if (selected_anchor_replacement != nullptr) {
            add_drop(selected_anchor_replacement->state);
        }
        for (std::size_t index = 0; index < drop_count; ++index) {
            const DroppedReference& drop = drops[index];
            if (!state_store->valid(drop.state) ||
                state_store->checkpoint_references(drop.state) != drop.count) {
                continue;
            }
            const StateReplicaResidency residency = state_store->residency(drop.state);
            if (residency == StateReplicaResidency::DeviceOnly ||
                residency == StateReplicaResidency::Both) {
                ++replaced_private.device.state_slots;
            }
            if (residency == StateReplicaResidency::HostOnly ||
                residency == StateReplicaResidency::Both) {
                ++replaced_private.host.state_slots;
            }
        }
    }
    detail::PhysicalResources replaced_shared;
    if (publish_shared && replacement != nullptr) {
        replaced_shared =
            owner_exclusive_resources(shared_prefix_states[ContractAccess::index(*replacement)]);
    }
    if (replaced_shared.device.state_slots > state_store->device_occupied()) {
        throw std::logic_error("shared capture replacement exceeds Device State occupancy");
    }
    const std::uint32_t device_state_after_preparation =
        state_store->device_occupied() - replaced_shared.device.state_slots;
    const bool device_destination_available =
        assessment.recycles_private_state ||
        device_state_after_preparation < state_store->device_capacity();
    if (device_destination_available || host_state_images == nullptr) {
        assessment.state_placement = qwen3_5::CaptureStatePlacement::DeviceFork;
        added.device.state_slots   = 1;
    } else {
        // A capture must not require a third Device image when the active image and a retained
        // checkpoint already occupy the C+H pool.  Snapshot the frozen logical checkpoint to
        // Host, then transfer ownership of its unchanged Device replica to the continuing active
        // identity.  This preserves both logical checkpoints without assigning fixed slot roles.
        assessment.state_placement = qwen3_5::CaptureStatePlacement::HostSnapshot;
        added.host.state_slots     = 1;
    }
    if (std::getenv("NINFER_CAPTURE_PROBE") != nullptr) {
        // Printed AT the decision, because the earlier probe cannot see these: `replaced_shared` is
        // declared below it. The first three runs of the host-snapshot scenario read `placement=0` with
        // `dev_occ == dev_cap`, which is only possible if a replacement frees a slot -- so read the
        // value the decision actually uses instead of inferring it.
        std::fprintf(stderr,
                     "[capture] placement-decision lane=%u placement=%d occupied=%u capacity=%u "
                     "replaced_shared_slots=%u after_preparation=%u host_images=%d\n",
                     lane, static_cast<int>(assessment.state_placement), state_store->device_occupied(),
                     state_store->device_capacity(), replaced_shared.device.state_slots,
                     device_state_after_preparation,
                     static_cast<int>(host_state_images != nullptr));
        std::fflush(stderr);
    }
    const detail::PhysicalResources replaced =
        checked_resource_sum(replaced_private, replaced_shared);
    assessment.implementation->capacity_preparation_removed = replaced_shared;
    assessment.implementation->demand                       = detail::PhysicalDemand{
                              .reservation_added  = added,
                              .reservation_credit = replaced_shared,
                              .final_removed      = replaced,
                              .final_added        = added,
    };
    if (assessment.recycles_private_state) {
        if (assessment.state_placement != qwen3_5::CaptureStatePlacement::DeviceFork) {
            throw std::logic_error("recycled rewrite capture selected Host placement");
        }
        if (replaced_private.device.state_slots == 0) {
            throw std::logic_error("recycled rewrite capture has no Device state replacement");
        }
        if (assessment.implementation->demand.reservation_credit.device.state_slots ==
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("capture StateImage reservation credit overflow");
        }
        ++assessment.implementation->demand.reservation_credit.device.state_slots;
    }
    assessment.implementation->demand.physical_peak_additional =
        positive_resource_difference(assessment.implementation->demand.reservation_added,
                                     assessment.implementation->demand.reservation_credit);
    assessment.implementation->active_entitlement_delta.removed =
        checked_resource_sum(active_removed, replaced_private);
    if (publish_private && !publish_shared) {
        assessment.implementation->active_entitlement_delta.added = added;
    }
    assessment.transfer_requirements.reserve(3);
    if (assessment.state_placement == qwen3_5::CaptureStatePlacement::HostSnapshot) {
        assessment.transfer_requirements.push_back(state_transfer_requirement(
            state_images->host_layout(), runtime::ContextTransferDirection::DeviceToHost));
    } else if (is_masked_draft_backend(speculative_backend)) {
        assessment.transfer_requirements.push_back(state_transfer_requirement(
            state_images->host_layout(), runtime::ContextTransferDirection::DeviceToDevice, true));
    }
    if (added.device.main_kv_pages != 0) {
        assessment.transfer_requirements.push_back(kv_transfer_requirement(
            runtime::ContextResourceClass::MainKV,
            runtime::ContextTransferDirection::DeviceToDevice,
            plan_host_kv_page_layout(text_kv_pages->physical_pool().geometry()),
            added.device.main_kv_pages));
    }
    if (added.device.backend_kv_pages != 0) {
        assessment.transfer_requirements.push_back(kv_transfer_requirement(
            runtime::ContextResourceClass::BackendKV,
            runtime::ContextTransferDirection::DeviceToDevice,
            plan_host_kv_page_layout(backend_kv_pages->physical_pool().geometry()),
            added.device.backend_kv_pages));
    }
    assessment.needs_transfer = !assessment.transfer_requirements.empty();
    assessment.physically_feasible =
        physical_peak_fits(assessment.implementation->demand.physical_peak_additional);
    if (publish_shared) {
        std::vector<runtime::ContextTransferRequirement> recovery;
        recovery.reserve(3);
        if (assessment.state_placement == qwen3_5::CaptureStatePlacement::HostSnapshot) {
            recovery.push_back(state_transfer_requirement(
                state_images->host_layout(), runtime::ContextTransferDirection::HostToDevice));
        } else if (is_masked_draft_backend(speculative_backend)) {
            recovery.push_back(state_transfer_requirement(
                state_images->host_layout(), runtime::ContextTransferDirection::DeviceToDevice,
                true));
        }
        if (text_snapshot_shape->copied_pages() != 0) {
            recovery.push_back(kv_transfer_requirement(
                runtime::ContextResourceClass::MainKV,
                runtime::ContextTransferDirection::DeviceToDevice,
                plan_host_kv_page_layout(text_kv_pages->physical_pool().geometry()),
                text_snapshot_shape->copied_pages()));
        }
        if (backend_snapshot_shape && backend_snapshot_shape->copied_pages() != 0) {
            recovery.push_back(kv_transfer_requirement(
                runtime::ContextResourceClass::BackendKV,
                runtime::ContextTransferDirection::DeviceToDevice,
                plan_host_kv_page_layout(backend_kv_pages->physical_pool().geometry()),
                backend_snapshot_shape->copied_pages()));
        }
        assessment.projected_recovery_work.reserve(2);
        assessment.projected_recovery_work.push_back(
            recovery_alternative_work({}, assessment.protected_rebuild_work));
        assessment.projected_recovery_work.push_back(recovery_alternative_work(recovery));
    }
    return assessment;
}

std::unique_ptr<CapturePressureCandidateImpl>
ProgramImpl::make_capture_physical_candidate(const CaptureAssessment& assessment) const {
    if (assessment.implementation == nullptr || !assessment.publishes_shared ||
        assessment.frontier == 0) {
        throw std::invalid_argument("capture pressure candidate is incomplete");
    }
    auto details                              = std::make_unique<CapturePressureCandidateImpl>();
    details->planning_revision                = resource_revision_;
    details->summary.prompt_tokens            = assessment.frontier;
    details->demand                           = assessment.implementation->demand;
    details->transfer_requirements            = assessment.transfer_requirements;
    details->needs_transfer                   = assessment.needs_transfer;
    details->identity_pressure_deficit        = materialization_deficit(*details);
    details->identity_assessment.machine_work = materialization_machine_work(*details, {}, {});
    details->identity_assessment.physical_status =
        physical_peak_fits(details->demand.physical_peak_additional)
            ? runtime::MaterializationPhysicalStatus::Feasible
            : runtime::MaterializationPhysicalStatus::Infeasible;
    details->identity_assessment.source_mode = runtime::PrivateSourceMode::ConsumeToActive;
    details->identity_assessment.expandable  = details->identity_assessment.physical_status !=
                                              runtime::MaterializationPhysicalStatus::Feasible;
    details->identity_assessment.projection_work = 1U + details->transfer_requirements.size();
    std::uint64_t digest                         = 1469598103934665603ULL;
    const auto mix                               = [&](std::uint64_t value) {
        digest ^= value;
        digest *= 1099511628211ULL;
    };
    mix(resource_revision_.value);
    mix(assessment.frontier);
    mix(static_cast<std::uint8_t>(details->identity_assessment.physical_status));
    details->identity_assessment.assessment_digest = digest;
    return details;
}

void ProgramImpl::skip_capture(CaptureOffer&& offer) {
    if (!valid_capture_offer(offer)) { throw std::logic_error("capture offer is not skippable"); }
    const std::uint32_t lane = ContractAccess::lane(offer).value;
    ContractAccess::consume(offer);
    RequestControl::Prefill& prefill = *requests[lane].prefill;
    prefill.pending_capture_offer    = 0;
    ++prefill.next_capture;
    if (prefill.cursor == prefill.prompt_tokens &&
        requests[lane].lifecycle != Lifecycle::Prefilling) {
        requests[lane].prefill.reset();
    }
}

runtime::ContextTransactionReserveStatus
ProgramImpl::reserve_active_capture(CaptureOffer&& offer, const SharedPrefixHandle* exact_shared,
                                    const SharedPrefixHandle* replacement,
                                    std::optional<runtime::CheckpointRef> private_replacement,
                                    bool permit_shared_publication,
                                    runtime::CancellationFlagView cancellation) {
    return reserve_active_capture_impl(std::move(offer), exact_shared, replacement,
                                       private_replacement, permit_shared_publication, std::nullopt,
                                       cancellation);
}

runtime::ContextTransactionReserveStatus ProgramImpl::reserve_active_capture_with_pressure(
    CaptureOffer&& offer, const SharedPrefixHandle* exact_shared,
    const SharedPrefixHandle* replacement,
    std::optional<runtime::CheckpointRef> private_replacement, bool permit_shared_publication,
    CapturePressureCandidate&& pressure, runtime::CancellationFlagView cancellation) {
    std::optional<CapturePressureCandidate> owned;
    owned.emplace(std::move(pressure));
    return reserve_active_capture_impl(std::move(offer), exact_shared, replacement,
                                       private_replacement, permit_shared_publication,
                                       std::move(owned), cancellation);
}

runtime::ContextTransactionReserveStatus ProgramImpl::reserve_active_capture_impl(
    CaptureOffer&& offer, const SharedPrefixHandle* exact_shared,
    const SharedPrefixHandle* replacement,
    std::optional<runtime::CheckpointRef> private_replacement, bool permit_shared_publication,
    std::optional<CapturePressureCandidate> pressure, runtime::CancellationFlagView cancellation) {
    // Contention is not corruption. Another context transaction, or an unsettled state
    // fork left by a shared-prefix reuse, owns the program; a capture is optional
    // retention, and §12 invariant 15 requires a retention/capture failure to degrade to
    // skip so the active lane still reaches a finite terminal state. Throwing here failed
    // the whole batch with HTTP 500 (reproduced: 4 concurrent sessions sharing a large
    // prefix, all four killed by "WORKER RECOVER: capture transaction is not reservable").
    if (has_context_transaction() || has_unsettled_state_fork()) {
        skip_capture(std::move(offer));
        return runtime::ContextTransactionReserveStatus::Aborted;
    }
    if (!valid_capture_offer(offer)) {
        // A DISTINCT message on purpose. The contention branch above and this one used to share
        // "capture transaction is not reservable", which is the string the pre-fix HTTP 500 was
        // reported as ("WORKER RECOVER: capture transaction is not reservable") -- so the evidence
        // "that line no longer appears" could not tell the fixed branch from this one, which still
        // throws. A stale offer is an internal-consistency failure, not contention, and it is not
        // expected on this path; keep it loud and separable.
        throw std::logic_error("capture offer is stale (invalid for this program state)");
    }
    if (cancellation.requested()) {
        skip_capture(std::move(offer));
        return runtime::ContextTransactionReserveStatus::Aborted;
    }
    const CaptureAssessment assessment = inspect_capture(
        offer, exact_shared, replacement, private_replacement, permit_shared_publication, "reserve");
    if (!assessment.publishes_private && !assessment.publishes_shared) {
        skip_capture(std::move(offer));
        return runtime::ContextTransactionReserveStatus::Aborted;
    }
    const CapturePressureCandidateImpl* pressure_details =
        pressure && pressure->impl_ ? pressure->impl_.get() : nullptr;
    if (pressure &&
        (pressure_details == nullptr || pressure_details->planning_revision != resource_revision_ ||
         pressure_details->summary.prompt_tokens != assessment.frontier ||
         pressure_details->blocked_host_allocation_bytes != 0 ||
         !physical_peak_fits(pressure_details->demand.physical_peak_additional))) {
        skip_capture(std::move(offer));
        return runtime::ContextTransactionReserveStatus::Aborted;
    }
    if (!pressure && !assessment.physically_feasible) {
        skip_capture(std::move(offer));
        return runtime::ContextTransactionReserveStatus::Aborted;
    }

    const std::uint32_t lane         = ContractAccess::lane(offer).value;
    RequestControl::Prefill& prefill = *requests[lane].prefill;
    SequenceState& sequence          = active_sequence(lane);
    ActiveCaptureTransaction transaction;
    transaction.id                  = ContractAccess::id(offer);
    transaction.lane                = lane;
    transaction.lane_epoch          = lane_epochs[lane];
    transaction.group               = prefill.capture_groups[prefill.next_capture];
    transaction.publish_private     = assessment.publishes_private;
    transaction.publish_shared      = assessment.publishes_shared;
    transaction.private_replacement = private_replacement;
    transaction.resource_delta      = detail::PhysicalDelta{
             .removed = assessment.implementation->demand.final_removed,
             .added   = assessment.implementation->demand.final_added,
    };
    transaction.active_entitlement_delta = assessment.implementation->active_entitlement_delta;
    transaction.capacity_preparation_removed =
        assessment.implementation->capacity_preparation_removed;
    transaction.recycles_private_state = assessment.recycles_private_state;
    transaction.state_placement        = assessment.state_placement;
    transaction.transfer_requirements  = assessment.transfer_requirements;
    if (pressure_details != nullptr) {
        if (pressure_details->pressure_options.size() !=
                pressure_details->pressure_owner_ids.size() ||
            pressure_details->pressure_options.size() !=
                pressure_details->pressure_indices.size() ||
            pressure_details->pressure_options.size() !=
                pressure_details->pressure_generations.size() ||
            pressure_details->shared_pressure_options.size() !=
                pressure_details->shared_pressure_owner_ids.size() ||
            pressure_details->shared_pressure_options.size() !=
                pressure_details->shared_pressure_indices.size() ||
            pressure_details->shared_pressure_options.size() !=
                pressure_details->shared_pressure_generations.size()) {
            throw std::logic_error("capture pressure plan is not row aligned");
        }
        transaction.victim_indices     = pressure_details->pressure_indices;
        transaction.victim_generations = pressure_details->pressure_generations;
        transaction.pressure_results.resize(pressure_details->pressure_options.size());
        transaction.pressure.reserve(pressure_details->pressure_options.size());
        for (std::size_t index = 0; index < pressure_details->pressure_options.size(); ++index) {
            transaction.pressure_results[index].owner = pressure_details->pressure_owner_ids[index];
            transaction.pressure_results[index].final_summary.emplace();
            transaction.pressure_results[index].final_summary->long_anchors.reserve(
                continuation_states[transaction.victim_indices[index]].long_anchors.size());
            transaction.pressure.push_back(MaterializationTransaction::PressureWork{
                .option                  = pressure_details->pressure_options[index],
                .continuation_index      = transaction.victim_indices[index],
                .continuation_generation = transaction.victim_generations[index],
            });
            prepare_pressure_bookkeeping(transaction.pressure.back());
        }
        transaction.shared_victim_indices     = pressure_details->shared_pressure_indices;
        transaction.shared_victim_generations = pressure_details->shared_pressure_generations;
        transaction.shared_pressure_results.resize(
            pressure_details->shared_pressure_options.size());
        transaction.shared_pressure.reserve(pressure_details->shared_pressure_options.size());
        for (std::size_t index = 0; index < pressure_details->shared_pressure_options.size();
             ++index) {
            transaction.shared_pressure_results[index].owner =
                pressure_details->shared_pressure_owner_ids[index];
            const std::uint32_t victim = transaction.shared_victim_indices[index];
            if (replacement != nullptr && ContractAccess::index(*replacement) == victim) {
                throw std::logic_error("capture logical replacement is duplicated by pressure");
            }
            transaction.shared_pressure.push_back(MaterializationTransaction::PressureWork{
                .option                  = pressure_details->shared_pressure_options[index],
                .continuation_index      = victim,
                .continuation_generation = transaction.shared_victim_generations[index],
                .shared_owner            = true,
            });
            prepare_pressure_bookkeeping(transaction.shared_pressure.back());
        }
    }
    if (transaction.publish_private) {
        transaction.active_summary.long_anchors.reserve(sequence.long_anchors.capacity());
    }
    transaction.transfer_observations.reserve(
        3U * (transaction.pressure.size() + transaction.shared_pressure.size()) + 3U);
    ContractAccess::consume(offer);

    try {
        if (transaction.publish_shared) {
            if (replacement != nullptr) {
                const std::uint32_t index = ContractAccess::index(*replacement);
                if (index >= shared_prefix_capacity ||
                    shared_prefix_slots[index].role != SharedPrefixSlotRole::Catalogued ||
                    shared_prefix_states[index].active_references != 0) {
                    throw std::logic_error("shared capture replacement changed before reserve");
                }
                transaction.shared_index           = index;
                transaction.replaces_shared        = true;
                transaction.replacement_generation = shared_prefix_slots[index].generation;
                shared_prefix_slots[index].role    = SharedPrefixSlotRole::ReservedReplacement;
                // Counted HERE, at the decision, because this is the only place that knows a catalogued
                // shared owner is being displaced. Nothing else reported it -- see the field's comment.
                ++shared_replacements_;
            } else {
                for (std::uint32_t index = 0; index < shared_prefix_capacity; ++index) {
                    if (shared_prefix_slots[index].role == SharedPrefixSlotRole::Free) {
                        shared_prefix_slots[index].role = SharedPrefixSlotRole::ReservedCapture;
                        transaction.shared_index        = index;
                        break;
                    }
                }
            }
            if (!transaction.shared_index) {
                throw std::logic_error("shared capture descriptor was not reserved by policy");
            }
        }

        transaction.transfer_enqueue_pending = assessment.needs_transfer;
        advance_resource_revision();
        context_transaction_.emplace<ActiveCaptureTransaction>(std::move(transaction));
        return runtime::ContextTransactionReserveStatus::Reserved;
    } catch (...) {
        abort_active_capture(transaction);
        prefill.pending_capture_offer = 0;
        ++prefill.next_capture;
        throw;
    }
}

detail::PhysicalResources
ProgramImpl::release_checkpoint_reference(StateImageHandle checkpoint) noexcept {
    detail::PhysicalResources removed;
    if (!state_store->valid(checkpoint)) { return removed; }
    try {
        const StateReplicaResidency residency = state_store->residency(checkpoint);
        const std::uint32_t references        = state_store->checkpoint_references(checkpoint);
        if (references != 0) { state_store->release_checkpoint_reference(checkpoint); }
        if (state_store->checkpoint_references(checkpoint) != 0 ||
            state_store->source_pins(checkpoint) != 0) {
            return removed;
        }
        if (!state_store->release(checkpoint)) { return removed; }
        if (residency == StateReplicaResidency::DeviceOnly ||
            residency == StateReplicaResidency::Both) {
            removed.device.state_slots = 1;
        }
        if (residency == StateReplicaResidency::HostOnly ||
            residency == StateReplicaResidency::Both) {
            removed.host.state_slots = 1;
        }
    } catch (...) {}
    return removed;
}

detail::PhysicalResources
ProgramImpl::install_private_capture(SequenceState& sequence, const CaptureGroup& group,
                                     StateImageHandle checkpoint,
                                     std::optional<runtime::CheckpointRef> replacement) {
    detail::PhysicalResources removed;
    if (group.rewrite) {
        if (sequence.rewrite_state && *sequence.rewrite_state != checkpoint) {
            if (std::getenv("NINFER_CAPTURE_PROBE") != nullptr) {
                std::fprintf(stderr,
                             "[capture] install replaces an existing rewrite_state "
                             "frontier=%u new_frontier=%u refs=%d\n",
                             sequence.rewrite_checkpoint.frontier, group.frontier,
                             static_cast<int>(
                                 state_store->checkpoint_references(*sequence.rewrite_state)));
                std::fflush(stderr);
            }
            removed = checked_resource_sum(removed,
                                           release_checkpoint_reference(*sequence.rewrite_state));
        }
        state_store->retain_checkpoint_reference(checkpoint);
        if (std::getenv("NINFER_CAPTURE_PROBE") != nullptr) {
            std::fprintf(stderr,
                         "[capture] install rewrite_state=set refs=%d residency=%d aliases_write=%d "
                         "aliases_read=%d frontier=%u\n",
                         static_cast<int>(state_store->checkpoint_references(checkpoint)),
                         static_cast<int>(state_store->residency(checkpoint)),
                         static_cast<int>(checkpoint == sequence.state.write),
                         static_cast<int>(checkpoint == sequence.state.read), group.frontier);
            std::fflush(stderr);
        }
        sequence.rewrite_state      = checkpoint;
        sequence.rewrite_checkpoint = RewriteCheckpoint{
            .valid        = true,
            .kind         = *group.rewrite,
            .frontier     = group.frontier,
            .state_epoch  = state_store->content_epoch(checkpoint),
            .rebuild_work = validated_rebuild_work(group.identity->rebuild_work, group.frontier),
        };
    }
    if (group.long_anchor && context_cache.max_long_anchors_per_continuation.value_or(0) != 0) {
        const std::size_t capacity_limit = context_cache.max_long_anchors_per_continuation.value();
        validate_long_anchor_ordinals(sequence.long_anchors, capacity_limit);
        std::uint32_t ordinal = 0;
        if (sequence.long_anchors.size() == capacity_limit) {
            if (!replacement || replacement->kind != runtime::CheckpointKind::LongAnchor) {
                throw std::logic_error("full long-anchor set has no selected replacement");
            }
            const auto victim =
                std::find_if(sequence.long_anchors.begin(), sequence.long_anchors.end(),
                             [&](const LongAnchorCheckpoint& anchor) {
                                 return anchor.frontier == replacement->frontier &&
                                        anchor.ordinal == replacement->ordinal;
                             });
            if (victim == sequence.long_anchors.end()) {
                throw std::logic_error("selected long-anchor replacement changed");
            }
            ordinal = victim->ordinal;
            removed = checked_resource_sum(removed, release_checkpoint_reference(victim->state));
            sequence.long_anchors.erase(victim);
        } else {
            if (replacement) {
                throw std::logic_error("non-full long-anchor set has a replacement");
            }
            for (std::size_t candidate = 1; candidate <= capacity_limit; ++candidate) {
                if (std::none_of(sequence.long_anchors.begin(), sequence.long_anchors.end(),
                                 [candidate](const LongAnchorCheckpoint& anchor) {
                                     return anchor.ordinal == candidate;
                                 })) {
                    ordinal = static_cast<std::uint32_t>(candidate);
                    break;
                }
            }
        }
        if (ordinal == 0 || ordinal > capacity_limit) {
            throw std::logic_error("long-anchor capture has no valid ordinal");
        }
        state_store->retain_checkpoint_reference(checkpoint);
        sequence.long_anchors.push_back(LongAnchorCheckpoint{
            .state        = checkpoint,
            .frontier     = group.frontier,
            .ordinal      = ordinal,
            .state_epoch  = state_store->content_epoch(checkpoint),
            .rebuild_work = validated_rebuild_work(group.identity->rebuild_work, group.frontier),
        });
        validate_long_anchor_ordinals(sequence.long_anchors, capacity_limit);
    }
    return removed;
}

void ProgramImpl::prepare_active_capture(ActiveCaptureTransaction& transaction) {
    if (transaction.prepared || transaction.lane >= max_concurrency ||
        transaction.lane_epoch != lane_epochs[transaction.lane]) {
        throw std::logic_error("active capture capacity preparation is stale");
    }
    SequenceState& sequence = active_sequence(transaction.lane);
    if (transaction.publish_shared) {
        if (!transaction.shared_index || *transaction.shared_index >= shared_prefix_capacity) {
            throw std::logic_error("shared capture has no reserved descriptor");
        }
        SharedPrefixSlot& slot = shared_prefix_slots[*transaction.shared_index];
        if (transaction.replaces_shared) {
            if (transaction.replacement_removed ||
                slot.role != SharedPrefixSlotRole::ReservedReplacement ||
                slot.generation != transaction.replacement_generation) {
                throw std::logic_error("shared capture replacement changed before preparation");
            }
            if (!can_release_shared_prefix_state(*transaction.shared_index,
                                                 SharedPrefixSlotRole::ReservedReplacement)) {
                throw std::logic_error("shared capture replacement is not strictly releasable");
            }
            const detail::PhysicalResources removed = release_shared_prefix_state_strict(
                *transaction.shared_index, SharedPrefixSlotRole::ReservedReplacement);
            if (removed != transaction.capacity_preparation_removed) {
                throw std::logic_error("shared capture preparation release changed");
            }
            transaction.replacement_removed    = true;
            transaction.replacement_generation = slot.generation;
            slot.role                          = SharedPrefixSlotRole::ReservedCapture;
        } else if (slot.role != SharedPrefixSlotRole::ReservedCapture ||
                   transaction.capacity_preparation_removed != detail::PhysicalResources{}) {
            throw std::logic_error("shared capture vacant descriptor changed before preparation");
        }
    } else if (transaction.capacity_preparation_removed != detail::PhysicalResources{}) {
        throw std::logic_error("private-only capture has shared preparation resources");
    }

    transaction.source_state = sequence.state.write;
    if (transaction.state_placement == qwen3_5::CaptureStatePlacement::HostSnapshot) {
        if (transaction.recycles_private_state || host_state_images == nullptr) {
            throw std::logic_error("Host capture placement has no valid backing");
        }
        std::optional<StateImageHandle> destination = state_store->reserve_logical_destination();
        if (!destination) {
            throw std::logic_error("selected capture has no prepared logical State capacity");
        }
        transaction.destination_state = *destination;
    } else if (transaction.recycles_private_state) {
        if (!sequence.rewrite_state) {
            throw std::logic_error("recycled rewrite destination is unavailable");
        }
        transaction.destination_state = *sequence.rewrite_state;
        (void)state_store->recycle_checkpoint_destination(transaction.destination_state);
    } else {
        std::optional<StateImageHandle> destination = state_store->reserve_destination();
        if (!destination) {
            throw std::logic_error("selected capture has no prepared Device State capacity");
        }
        transaction.destination_state = *destination;
    }

    if (transaction.publish_shared) {
        if (!sequence.kv || sequence.state.fork_pending ||
            sequence.state.read != sequence.state.write ||
            state_store->role(sequence.state.write) != StateImageRole::ActiveMutable) {
            throw std::logic_error("active capture source is not an in-place writer");
        }
        trim_sequence_kv(sequence, sequence.text_kv_valid, backend_kv_valid(sequence));
        transaction.active_text_destination = text_kv_addresses->create_inactive();
        if (!transaction.active_text_destination) {
            throw std::logic_error("selected capture has no Text KV address descriptor");
        }
        transaction.text_snapshot.emplace(text_kv_addresses->prepare_active_snapshot(
            sequence.kv->text, *transaction.active_text_destination, sequence.text_kv_valid));
        if (sequence.kv->backend) {
            transaction.active_backend_destination = backend_kv_addresses->create_inactive();
            if (!transaction.active_backend_destination) {
                throw std::logic_error("selected capture has no Backend KV address descriptor");
            }
            transaction.backend_snapshot.emplace(backend_kv_addresses->prepare_active_snapshot(
                *sequence.kv->backend, *transaction.active_backend_destination,
                backend_kv_valid(sequence)));
        }
    }

    // W1-B: record the position the state is frozen at, before anything can move it. This is the
    // value the published entry's advertised frontier is compared against.
    //
    // `sequence.text_kv_valid`, NOT `execution_frontier`. A shared capture is offered and frozen
    // *mid-prefill*, and `execution_frontier` is written only at decode and forced commits -- during
    // a prompt prefill it is still 0. Comparing the advertised frontier against it therefore
    // counted every mid-prefill capture as a mismatch: the first version of this counter read 1/1
    // in the canary workload and I read that as a live hazard. The counter was measuring the wrong
    // quantity, not the engine doing something wrong. `text_kv_valid` is the position the
    // sequence's KV has been written to, i.e. what the frozen state corresponds to.
    //
    // Scope, per a 2026-09-25 review: every offer path triggers on the same cursor this holds, so
    // the comparison is equal by construction and can fire only on drift between offer and freeze.
    // It is a drift detector, not a check that the state matches the frontier it advertises.
    transaction.frozen_text_frontier = sequence.text_kv_valid;
    state_store->freeze(transaction.source_state);
    if (transaction.state_placement == qwen3_5::CaptureStatePlacement::DeviceFork) {
        const StateImageSelectors capture_fork =
            state_store->begin_fork(transaction.source_state, transaction.destination_state);
        // Fork completeness control (NINFER_FORK_COPY=1): the publishing lane continues in the
        // fork destination, and the ops that consume the selector pair write only the regions
        // they own -- anything else keeps the previous occupant's bytes. This site is the
        // *publisher's* continuation fork (the consumer-side sites are in prefill.cpp).
        if (diagnostic_control_enabled("NINFER_FORK_COPY")) {
            state_images->copy_slot(capture_fork.source, capture_fork.destination, device.stream);
        }
        sequence.state = ActiveStateBinding{.read         = transaction.source_state,
                                            .write        = transaction.destination_state,
                                            .fork_pending = true};
        refresh_state_views(sequence);
    }
    transaction.prepared = true;
}

void ProgramImpl::enqueue_active_capture_transfers(ActiveCaptureTransaction& transaction) {
    if (!transaction.prepared || !transaction.transfer_enqueue_pending ||
        transaction.transfer_submitted) {
        throw std::logic_error("active capture transfer batch is not enqueueable");
    }
    context_source_ready_.record(device.stream);
    context_source_ready_.wait(device.transfer_stream);
    if (transaction.state_placement == qwen3_5::CaptureStatePlacement::HostSnapshot) {
        start_context_transfer_timer(runtime::ContextResourceClass::State);
        std::optional<StateImageTransfer> snapshot =
            state_store->begin_device_to_host(transaction.source_state, device.transfer_stream);
        if (!snapshot) {
            throw std::logic_error("selected Host capture has no prepared State target");
        }
        transaction.state_snapshot.emplace(std::move(*snapshot));
        stop_context_transfer_timer(runtime::ContextResourceClass::State);
        transaction.transfer_timer_mask |=
            1U << context_resource_index(runtime::ContextResourceClass::State);
    } else if (is_masked_draft_backend(speculative_backend)) {
        const StateImageSelectors state_fork =
            state_store->selectors(transaction.source_state, transaction.destination_state);
        start_context_transfer_timer(runtime::ContextResourceClass::State);
        state_images->copy_dflash_local(state_fork.source, state_fork.destination,
                                        device.transfer_stream);
        stop_context_transfer_timer(runtime::ContextResourceClass::State);
        transaction.transfer_timer_mask |=
            1U << context_resource_index(runtime::ContextResourceClass::State);
    }
    if (transaction.text_snapshot && transaction.text_snapshot->needs_tail_copy()) {
        start_context_transfer_timer(runtime::ContextResourceClass::MainKV);
        decoder->text_kv.page_pool().copy_page(
            text_kv_addresses->active_snapshot_tail_source(*transaction.text_snapshot),
            text_kv_addresses->active_snapshot_tail_destination(*transaction.text_snapshot),
            device.transfer_stream);
        stop_context_transfer_timer(runtime::ContextResourceClass::MainKV);
        transaction.transfer_timer_mask |=
            1U << context_resource_index(runtime::ContextResourceClass::MainKV);
        ++transaction.operations.partial_tail_cow_pages;
    }
    if (transaction.backend_snapshot && transaction.backend_snapshot->needs_tail_copy()) {
        start_context_transfer_timer(runtime::ContextResourceClass::BackendKV);
        backend_kv_cache()->page_pool().copy_page(
            backend_kv_addresses->active_snapshot_tail_source(*transaction.backend_snapshot),
            backend_kv_addresses->active_snapshot_tail_destination(*transaction.backend_snapshot),
            device.transfer_stream);
        stop_context_transfer_timer(runtime::ContextResourceClass::BackendKV);
        transaction.transfer_timer_mask |=
            1U << context_resource_index(runtime::ContextResourceClass::BackendKV);
        ++transaction.operations.partial_tail_cow_pages;
    }
    context_completion_.record(device.transfer_stream);
    transaction.transfer_enqueue_pending = false;
    transaction.transfer_submitted       = true;
}

void ProgramImpl::abort_active_capture(ActiveCaptureTransaction& transaction) noexcept {
    if (transaction.lane < max_concurrency &&
        active_continuations[transaction.lane] < continuation_capacity) {
        SequenceState& sequence = active_sequence(transaction.lane);
        if (transaction.backend_snapshot) {
            backend_kv_addresses->abort_active_snapshot(*transaction.backend_snapshot);
            transaction.backend_snapshot.reset();
        }
        if (transaction.text_snapshot) {
            text_kv_addresses->abort_active_snapshot(*transaction.text_snapshot);
            transaction.text_snapshot.reset();
        }
        if (transaction.active_backend_destination &&
            backend_kv_addresses->valid(*transaction.active_backend_destination)) {
            if (!backend_kv_addresses->release(*transaction.active_backend_destination)) {
                note_nonstrict_release_refusal("abort-backend-destination");
            }
        }
        if (transaction.active_text_destination &&
            text_kv_addresses->valid(*transaction.active_text_destination)) {
            if (!text_kv_addresses->release(*transaction.active_text_destination)) {
                note_nonstrict_release_refusal("abort-text-destination");
            }
        }
        if (transaction.state_snapshot) {
            state_store->abort_transfer(std::move(*transaction.state_snapshot));
            transaction.state_snapshot.reset();
        }
        if (state_store->valid(transaction.source_state) &&
            state_store->valid(transaction.destination_state)) {
            try {
                if (transaction.state_placement == qwen3_5::CaptureStatePlacement::HostSnapshot) {
                    if (!state_store->release(transaction.destination_state)) {
                        note_nonstrict_release_refusal("abort-snapshot-destination");
                    }
                } else {
                    if (sequence.state.fork_pending &&
                        sequence.state.read == transaction.source_state &&
                        sequence.state.write == transaction.destination_state) {
                        state_store->abort_fork(transaction.source_state,
                                                transaction.destination_state);
                        sequence.state = ActiveStateBinding{.read  = transaction.source_state,
                                                            .write = transaction.source_state};
                    }
                    if (transaction.recycles_private_state) {
                        // W1-B, and now #11(a)'s disposal. The fork destination IS the sequence's rewrite
                        // checkpoint (`destination_state = *sequence.rewrite_state`), so an abort used to
                        // restore the checkpoint's *old* content epoch -- over content the fork may have
                        // written, which is the hazard W1-B named. It no longer does: the checkpoint is
                        // DROPPED here instead. The branch is unreachable by
                        // construction (plan.md §2 item 3), and the epoch restore is the hazard the
                        // counter here used to watch for; dropping is safe whether or not the branch is
                        // ever entered, and costs at most one cache entry -- a checkpoint whose bytes may
                        // be dirty is worth less than one whose bytes are known good.
                        //
                        // "Release" is spelled out because the literal version is unsafe: after
                        // `recycle_checkpoint_destination` the object is `ReservedDestination` with
                        // `checkpoint_references == 0`, so a bare release would leave
                        // `sequence.rewrite_state` pointing at it with `rewrite_checkpoint.valid` still
                        // true -- and this file's own guard tests exactly those two fields, without a
                        // `valid()` check, so a later `release_checkpoint_reference` would `require()` a
                        // stale handle.
                        const StateImageHandle recycled = transaction.destination_state;
                        // The reset and the count hang on the release SUCCEEDING. Resetting the sequence's
                        // handle after a refused release would drop the handle while the occupancy stayed
                        // -- #9's exact shape, created by the fix -- and counting a "drop" that did not
                        // happen is the kind of instrument this change removes elsewhere.
                        //
                        // `recycled_checkpoint_drops_` can only read zero: this branch is unreachable
                        // (`recycles_private_state` requires a state the planner never produces). It has no
                        // denominator -- it is not an instrument that can report a rate -- and it is kept
                        // for one reason only: if the reachability argument is ever refuted, the first
                        // evidence will be this counter firing. Named here as unreachable rather than
                        // presented as something that measures.
                        if (state_store->release(recycled)) {
                            if (sequence.rewrite_state && *sequence.rewrite_state == recycled) {
                                sequence.rewrite_state.reset();
                                sequence.rewrite_checkpoint = {};
                            }
                            ++recycled_checkpoint_drops_;
                            if (recycled_checkpoint_drops_ <= 8ULL ||
                                recycled_checkpoint_drops_ % 512ULL == 0ULL) {
                                std::fprintf(stderr,
                                             "[capture] recycled-checkpoint DROPPED on abort lane=%u "
                                             "count=%llu (not restored: its bytes may hold the fork's "
                                             "writes)\n",
                                             sequence.lane,
                                             static_cast<unsigned long long>(recycled_checkpoint_drops_));
                                std::fflush(stderr);
                            }
                        } else {
                            // A refused release leaves an inconsistent binding that this arm does NOT
                            // repair: `recycle_checkpoint_destination` has already turned the object into
                            // `ReservedDestination` with `checkpoint_references == 0`, so
                            // `sequence.rewrite_state` still names it while `rewrite_checkpoint.valid` stays
                            // true -- and a later `release_checkpoint_reference` would `require()` a role
                            // and refcount that no longer match. The gating above is still right (dropping
                            // the handle on a refusal is #9's shape), but the residual risk is named here
                            // rather than left to be discovered. Unreachable in practice: the arm requires
                            // a state the planner never produces.
                            note_nonstrict_release_refusal("abort-recycled-checkpoint");
                        }
                    } else {
                        if (!state_store->release(transaction.destination_state)) {
                            note_nonstrict_release_refusal("abort-destination");
                        }
                    }
                }
                state_store->thaw(transaction.source_state);
                refresh_state_views(sequence);
            } catch (...) {}
        }
    }
    if (transaction.shared_index && *transaction.shared_index < shared_prefix_capacity) {
        SharedPrefixSlot& slot = shared_prefix_slots[*transaction.shared_index];
        // A reserved role belonging to a transaction that is going away is never left reserved
        // (shared_slot_release.h, enumerated and unit-tested). The chain this replaces matched only
        // three combinations of (replaces_shared, replacement_removed, role): a slot in either of the
        // other two kept its KV addresses and its state checkpoint reference with no owner, and
        // `fail_all_cleanup` releases only `Catalogued` slots, so nothing could free it -- the
        // 2026-09-25 leak candidate (#9).
        //
        // The generation guard is kept where the old chain had it: for a replacement flow the slot may
        // have been re-reserved by a later transaction, and touching that one would corrupt it.
        const bool ours = !transaction.replaces_shared ||
                          slot.generation == transaction.replacement_generation;
        if (ours) {
            switch (resolve_shared_slot_release(transaction.replaces_shared,
                                                transaction.replacement_removed, slot.role)) {
            case SharedSlotReleaseAction::Free: slot.role = SharedPrefixSlotRole::Free; break;
            case SharedSlotReleaseAction::Catalogue:
                slot.role = SharedPrefixSlotRole::Catalogued;
                break;
            case SharedSlotReleaseAction::Leave: break;
            }
        }
    }
    transaction.prepared = false;
}

ActiveCaptureResult ProgramImpl::publish_active_capture(ActiveCaptureTransaction& transaction) {
    if (!transaction.prepared || transaction.lane >= max_concurrency ||
        transaction.lane_epoch != lane_epochs[transaction.lane] || transaction.published) {
        throw std::logic_error("active capture transaction is stale");
    }
    SequenceState& sequence          = active_sequence(transaction.lane);
    RequestControl& request          = requests[transaction.lane];
    RequestControl::Prefill& prefill = *request.prefill;
    if (prefill.pending_capture_offer != transaction.id ||
        prefill.next_capture >= prefill.capture_groups.size()) {
        throw std::logic_error("active capture offer ownership changed");
    }

    if (transaction.state_placement == qwen3_5::CaptureStatePlacement::HostSnapshot) {
        if (!transaction.state_snapshot || sequence.state.fork_pending ||
            sequence.state.read != transaction.source_state ||
            sequence.state.write != transaction.source_state) {
            throw std::logic_error("Host capture snapshot is not publishable");
        }
        state_store->publish_transfer(std::move(*transaction.state_snapshot), true);
        transaction.state_snapshot.reset();
        state_store->split_device_replica_identity(transaction.source_state,
                                                   transaction.destination_state);
        sequence.state = ActiveStateBinding{.read  = transaction.destination_state,
                                            .write = transaction.destination_state};
        refresh_state_views(sequence);
    }

    std::optional<SequenceKVBundle> shared_bundle;
    if (transaction.publish_shared) {
        shared_bundle = *sequence.kv;
        text_kv_addresses->commit_active_snapshot(std::move(*transaction.text_snapshot),
                                                  device.stream);
        transaction.text_snapshot.reset();
        SequenceKVBundle active_bundle{.text = *transaction.active_text_destination};
        transaction.active_text_destination.reset();
        if (transaction.backend_snapshot) {
            backend_kv_addresses->commit_active_snapshot(std::move(*transaction.backend_snapshot),
                                                         device.stream);
            transaction.backend_snapshot.reset();
            active_bundle.backend = *transaction.active_backend_destination;
            transaction.active_backend_destination.reset();
        }
        sequence.kv = active_bundle;
        bind_sequence_kv(sequence);
        const std::uint32_t backend_materialized =
            speculative_backend == SpeculativeBackend::Mtp
                ? std::min(capacity,
                           prefill.prompt_tokens + (prefill.initial_mtp_extent == 0
                                                        ? 0U
                                                        : prefill.initial_mtp_extent - 1U))
            : speculative_backend == SpeculativeBackend::DFlash ? prefill.prompt_tokens
                                                                : 0U;
        ensure_sequence_kv_mapped(sequence, prefill.prompt_tokens, backend_materialized);
    }

    detail::PhysicalResources removed = transaction.capacity_preparation_removed;
    if (transaction.publish_shared) {
        state_store->retain_checkpoint_reference(transaction.source_state);
    }
    if (transaction.publish_private) {
        if (transaction.recycles_private_state) {
            if (!sequence.rewrite_state ||
                *sequence.rewrite_state != transaction.destination_state ||
                !sequence.rewrite_checkpoint.valid) {
                throw std::logic_error("recycled rewrite metadata changed before publication");
            }
            sequence.rewrite_state.reset();
            sequence.rewrite_checkpoint        = {};
            sequence.rewrite_checkpoint_hidden = {};
            removed.device.state_slots         = 1;
        }
        removed = checked_resource_sum(
            removed, install_private_capture(sequence, transaction.group, transaction.source_state,
                                             transaction.private_replacement));
    }
    if (transaction.replaces_shared) {
        if (!transaction.shared_index) {
            throw std::logic_error("shared replacement has no descriptor");
        }
        const std::uint32_t index = *transaction.shared_index;
        if (!transaction.replacement_removed ||
            shared_prefix_slots[index].role != SharedPrefixSlotRole::ReservedCapture ||
            shared_prefix_slots[index].generation != transaction.replacement_generation) {
            throw std::logic_error("shared replacement generation changed before publication");
        }
    }
    if (removed != transaction.resource_delta.removed) {
        throw std::logic_error("active capture replacement effect changed after reservation");
    }

    const detail::PhysicalResources private_replacement_removed =
        checked_resource_difference(removed, transaction.capacity_preparation_removed);
    request.optional_resources =
        checked_resource_difference(request.optional_resources, private_replacement_removed);
    if (transaction.publish_private && !transaction.publish_shared) {
        request.optional_resources =
            checked_resource_sum(request.optional_resources, transaction.resource_delta.added);
    }
    request.active_resources = checked_resource_sum(
        checked_resource_difference(request.active_resources,
                                    transaction.active_entitlement_delta.removed),
        transaction.active_entitlement_delta.added);

    if (transaction.state_placement == qwen3_5::CaptureStatePlacement::DeviceFork) {
        ++transaction.operations.state_forks;
    }
    ActiveCaptureResult out;
    out.status                         = runtime::ContextTransactionStatus::Published;
    out.capacity_preparation_committed = transaction.replacement_removed;
    if (transaction.publish_private) {
        populate_continuation_summary(sequence, transaction.active_summary);
        out.active_summary = std::move(transaction.active_summary);
    }
    out.victims               = std::move(transaction.pressure_results);
    out.shared_victims        = std::move(transaction.shared_pressure_results);
    out.transfer_observations = std::move(transaction.transfer_observations);
    out.operations            = transaction.operations;
    if (transaction.publish_shared) {
        if (!transaction.shared_index || !shared_bundle) {
            throw std::logic_error("shared capture publication has no reserved descriptor");
        }
        const std::uint32_t index = *transaction.shared_index;
        SharedPrefixSlot& slot    = shared_prefix_slots[index];
        SharedPrefixState& shared = shared_prefix_states[index];
        if (slot.role != SharedPrefixSlotRole::ReservedCapture || shared.kv || shared.identity) {
            throw std::logic_error("shared capture descriptor changed before publication");
        }
        shared.kv       = *shared_bundle;
        shared.state    = transaction.source_state;
        // W1-B (plan form): the epoch this entry's state is frozen at, recorded with the entry.
        shared.state_epoch = state_store->content_epoch(transaction.source_state);
        shared.identity = transaction.group.identity;
        shared.frontier = transaction.group.frontier;
        // Frontier audit (NINFER_MAT_DEBUG=1). CORRECTED 2026-09-25: this said the frozen state's
        // position "is the sequence's execution frontier", and that a difference meant cross-session
        // bleed "reproduced by tools/e2e/canary-e2e.py". Both halves were wrong. A shared capture is
        // frozen mid-prefill, where `execution_frontier` is still 0 -- so this print showed
        // `exec_frontier=0` beside a non-zero `group_frontier` for every mid-prefill capture, which
        // is not a mismatch but two different quantities. The position the state corresponds to is
        // the prefill cursor, and against it the recorded corpus is 62 of 62 agreeing
        // (`group_frontier == prefill_cursor`, `exec_frontier == 0` in all 62). No bleed was
        // reproduced from this condition by that suite, and the bleed it did reproduce is
        // attributable to the KV row selector (479c92c4).
        if (std::getenv("NINFER_MAT_DEBUG")) {
            std::fprintf(stderr,
                         "[mat-debug] SHARED-PUBLISH group_frontier=%u exec_frontier=%u "
                         "identity_frontier=%u prefill_cursor=%u prefill_prompt=%u\n",
                         transaction.group.frontier, sequence.execution_frontier,
                         transaction.group.identity ? transaction.group.identity->shortlist_key.frontier : 0U,
                         prefill.cursor, prefill.prompt_tokens);
            std::fflush(stderr);
        }
        // W1-B: the same condition, counted in every build. The entry advertises
        // `group.frontier` while its state was frozen at the sequence's execution frontier; when
        // those differ the entry does not correspond to the state behind it, and a session matching
        // only the shorter common boundary forks another session's later tokens. That was visible
        // only under NINFER_MAT_DEBUG, so the rate was never measured in a served run -- counted
        // here with its denominator, and deliberately NOT enforced: suppression is a cache-behaviour
        // change that needs the rate first, and a throw here would kill the worker (the wedge,
        // 2026-09-25).
        ++shared_publishes_;
        const bool frontier_matches =
            transaction.group.frontier == transaction.frozen_text_frontier;
        if (!frontier_matches) { ++shared_publish_frontier_mismatches_; }
        // The denominator prints too, and on a schedule that cannot swallow it: a mismatch-only
        // print leaves "no line" meaning either "no mismatch" or "no publishes", which is the one
        // distinction this counter exists to make.
        if (shared_publishes_ == 1U || shared_publishes_ % 512U == 0U || !frontier_matches) {
            std::fprintf(stderr,
                         "[capture] SHARED-FRONTIER advertised=%u frozen=%u exec=%u lane=%u "
                         "identity=%u mismatches=%llu/%llu\n",
                         transaction.group.frontier, transaction.frozen_text_frontier,
                         sequence.execution_frontier, transaction.lane,
                         transaction.group.identity
                             ? transaction.group.identity->shortlist_key.frontier
                             : 0U,
                         static_cast<unsigned long long>(shared_publish_frontier_mismatches_),
                         static_cast<unsigned long long>(shared_publishes_));
            std::fflush(stderr);
        }
        shared.backend_frontier =
            speculative_backend == SpeculativeBackend::Mtp      ? transaction.group.frontier - 1U
            : speculative_backend == SpeculativeBackend::DFlash ? transaction.group.frontier
                                                                : 0U;
        shared.rope_delta        = sequence.rope_delta;
        shared.tail_hidden_valid = sequence.tail_hidden_valid;
        shared.rebuild_work      = validated_rebuild_work(transaction.group.identity->rebuild_work,
                                                          transaction.group.frontier);
        shared.active_references = 1;
        sequence.shared_prefix_references.push_back(index);
        slot.role = SharedPrefixSlotRole::Catalogued;
        out.shared.emplace(SharedPrefixPublication{
            .handle  = ContractAccess::make_shared_prefix(this, index, slot.generation),
            .summary = shared_prefix_summary(shared),
        });
    }

    prefill.pending_capture_offer = 0;
    const bool post_begin_prompt_frontier_capture =
        prefill.cursor == prefill.prompt_tokens && request.lifecycle != Lifecycle::Prefilling;
    ++prefill.next_capture;
    if (post_begin_prompt_frontier_capture) { request.prefill.reset(); }
    transaction.published = true;
    return out;
}

ActiveCaptureResult
ProgramImpl::progress_active_capture_transaction(runtime::CancellationFlagView cancellation) {
    ActiveCaptureTransaction* transaction_ptr =
        std::get_if<ActiveCaptureTransaction>(&context_transaction_);
    if (transaction_ptr == nullptr) {
        throw std::logic_error("Program has no active capture transaction");
    }
    ActiveCaptureTransaction& transaction   = *transaction_ptr;
    PressureTransition& pressure_transition = transaction.pressure_transition;
    if (transaction.published) {
        throw std::logic_error("active capture terminal result was already returned");
    }
    const auto abort = [&]() -> ActiveCaptureResult {
        abort_active_capture(transaction);
        if (transaction.lane < max_concurrency && requests[transaction.lane].prefill) {
            RequestControl::Prefill& prefill = *requests[transaction.lane].prefill;
            const bool post_begin_prompt_frontier_capture =
                prefill.cursor == prefill.prompt_tokens &&
                requests[transaction.lane].lifecycle != Lifecycle::Prefilling;
            prefill.pending_capture_offer = 0;
            ++prefill.next_capture;
            if (post_begin_prompt_frontier_capture) { requests[transaction.lane].prefill.reset(); }
        }
        transaction.published = true;
        ActiveCaptureResult out;
        out.status                         = runtime::ContextTransactionStatus::Aborted;
        out.capacity_preparation_committed = transaction.replacement_removed;
        out.victims                        = std::move(transaction.pressure_results);
        out.shared_victims                 = std::move(transaction.shared_pressure_results);
        out.transfer_observations          = std::move(transaction.transfer_observations);
        out.operations                     = transaction.operations;
        return out;
    };
    const auto has_pressure = [&]() {
        return !transaction.pressure.empty() || !transaction.shared_pressure.empty();
    };
    const auto for_each_pending_pressure = [&](auto&& callback) {
        for (MaterializationTransaction::PressureWork& work : transaction.shared_pressure) {
            if (!work.completed) { callback(work); }
        }
        for (MaterializationTransaction::PressureWork& work : transaction.pressure) {
            if (!work.completed) { callback(work); }
        }
    };
    const auto collect_spill = [&](MaterializationTransaction::PressureWork& work) {
        transaction.operations.pressure_spill_pages =
            work.spill_pages > std::numeric_limits<std::uint64_t>::max() -
                                   transaction.operations.pressure_spill_pages
                ? std::numeric_limits<std::uint64_t>::max()
                : transaction.operations.pressure_spill_pages + work.spill_pages;
        work.spill_pages = 0;
        if (work.kv_demoted_to_host && !work.shared_owner) {
            ++transaction.operations.pressure_private_owners_demoted_kv;
            if (!work.state_transfer_published) {
                ++transaction.operations.pressure_private_owners_demoted_kv_only;
            }
        }
        work.kv_demoted_to_host       = false;
        work.state_transfer_published = false;
    };

    if (has_pressure() && pressure_transition.phase == PressureTransitionPhase::HostReleases) {
        if (cancellation.requested()) { return abort(); }
        for (std::size_t position = 0; position < transaction.shared_pressure.size(); ++position) {
            auto& work                     = transaction.shared_pressure[position];
            const std::uint32_t index      = transaction.shared_victim_indices[position];
            const std::uint64_t generation = transaction.shared_victim_generations[position];
            if (work.option.evicts_continuation) {
                if (index >= shared_prefix_capacity ||
                    shared_prefix_slots[index].role != SharedPrefixSlotRole::Catalogued ||
                    shared_prefix_slots[index].generation != generation ||
                    shared_prefix_states[index].active_references != 0) {
                    throw std::logic_error("capture shared pressure victim changed before release");
                }
                const detail::PhysicalResources exclusive =
                    owner_exclusive_resources(shared_prefix_states[index]);
                if (!can_release_shared_prefix_state(index, SharedPrefixSlotRole::Catalogued)) {
                    throw std::logic_error(
                        "capture shared pressure victim is not strictly releasable");
                }
                const detail::PhysicalResources released =
                    release_shared_prefix_state_strict(index, SharedPrefixSlotRole::Catalogued);
                if (released != exclusive ||
                    work.option.effect.added != detail::PhysicalResources{}) {
                    throw std::logic_error("capture shared pressure eviction changed");
                }
                work.committed_delta    = detail::PhysicalDelta{.removed = released};
                work.completed          = true;
                work.mutation_published = true;
                transaction.shared_pressure_results[position] = MaterializationSharedVictimResult{
                    .owner              = transaction.shared_pressure_results[position].owner,
                    .disposition        = runtime::VictimDisposition::Evicted,
                    .pressure_committed = true,
                };
            } else {
                publish_pressure_host_releases(work);
                if (work.completed) {
                    transaction.shared_pressure_results[position] =
                        MaterializationSharedVictimResult{
                            .owner       = transaction.shared_pressure_results[position].owner,
                            .disposition = runtime::VictimDisposition::Retained,
                            .pressure_committed = true,
                            .final_summary = shared_prefix_summary(shared_prefix_states[index]),
                        };
                }
            }
        }
        for (std::size_t position = 0; position < transaction.pressure.size(); ++position) {
            auto& work                     = transaction.pressure[position];
            const std::uint32_t index      = transaction.victim_indices[position];
            const std::uint64_t generation = transaction.victim_generations[position];
            if (work.option.evicts_continuation) {
                if (index >= continuation_capacity ||
                    continuation_slots[index].role != ContinuationSlotRole::Catalogued ||
                    continuation_slots[index].generation != generation ||
                    work.option.effect.added != detail::PhysicalResources{}) {
                    throw std::logic_error(
                        "capture private pressure victim changed before release");
                }
                if (!can_release_continuation_slot_strict(index)) {
                    throw std::logic_error("capture private victim is not strictly releasable");
                }
                const detail::PhysicalResources exclusive =
                    owner_exclusive_resources(continuation_states[index]);
                release_continuation_slot_strict(index);
                work.committed_delta    = detail::PhysicalDelta{.removed = exclusive};
                work.completed          = true;
                work.mutation_published = true;
                transaction.pressure_results[position] = MaterializationVictimResult{
                    .owner              = transaction.pressure_results[position].owner,
                    .disposition        = runtime::VictimDisposition::Evicted,
                    .pressure_committed = true,
                };
            } else {
                publish_pressure_host_releases(work);
                if (work.completed) {
                    transaction.pressure_results[position] = MaterializationVictimResult{
                        .owner              = transaction.pressure_results[position].owner,
                        .disposition        = runtime::VictimDisposition::Retained,
                        .pressure_committed = true,
                        .final_summary      = continuation_summary(continuation_states[index]),
                    };
                }
            }
        }
        pressure_transition.phase = PressureTransitionPhase::CopyPreparation;
    }

    if (has_pressure() && pressure_transition.phase == PressureTransitionPhase::CopyPreparation) {
        constexpr std::array resources{
            runtime::ContextResourceClass::State,
            runtime::ContextResourceClass::MainKV,
            runtime::ContextResourceClass::BackendKV,
        };
        context_source_ready_.record(device.stream);
        context_source_ready_.wait(device.transfer_stream);
        try {
            for (const runtime::ContextResourceClass resource : resources) {
                bool has_copy = false;
                for_each_pending_pressure([&](const auto& work) {
                    has_copy =
                        has_copy ||
                        std::any_of(work.option.transfer_requirements.begin(),
                                    work.option.transfer_requirements.end(),
                                    [&](const auto& requirement) {
                                        return requirement.resource == resource &&
                                               requirement.direction ==
                                                   runtime::ContextTransferDirection::DeviceToHost;
                                    });
                });
                if (has_copy) { start_context_transfer_timer(resource); }
                for_each_pending_pressure(
                    [&](auto& work) { prepare_pressure_work(work, resource); });
                if (!has_copy) { continue; }
                stop_context_transfer_timer(resource);
                const std::size_t resource_index = context_resource_index(resource);
                pressure_transition.timer_mask |= static_cast<std::uint8_t>(1U << resource_index);
                for_each_pending_pressure([&](const auto& work) {
                    for (const auto& requirement : work.option.transfer_requirements) {
                        if (requirement.resource != resource ||
                            requirement.direction !=
                                runtime::ContextTransferDirection::DeviceToHost) {
                            continue;
                        }
                        TransferWork& total = pressure_transition.transfer_work[resource_index];
                        total.payload_bytes =
                            requirement.work.payload_bytes >
                                    std::numeric_limits<std::uint64_t>::max() - total.payload_bytes
                                ? std::numeric_limits<std::uint64_t>::max()
                                : total.payload_bytes + requirement.work.payload_bytes;
                        const std::uint64_t operations =
                            static_cast<std::uint64_t>(total.copy_operations) +
                            requirement.work.copy_operations;
                        total.copy_operations =
                            operations > std::numeric_limits<std::uint32_t>::max()
                                ? std::numeric_limits<std::uint32_t>::max()
                                : static_cast<std::uint32_t>(operations);
                        const std::uint64_t pages =
                            static_cast<std::uint64_t>(
                                pressure_transition.transfer_pages[resource_index]) +
                            requirement.page_count;
                        pressure_transition.transfer_pages[resource_index] =
                            pages > std::numeric_limits<std::uint32_t>::max()
                                ? std::numeric_limits<std::uint32_t>::max()
                                : static_cast<std::uint32_t>(pages);
                        if (resource == runtime::ContextResourceClass::State) {
                            pressure_transition.state_images =
                                requirement.units > std::numeric_limits<std::uint64_t>::max() -
                                                        pressure_transition.state_images
                                    ? std::numeric_limits<std::uint64_t>::max()
                                    : pressure_transition.state_images + requirement.units;
                        }
                    }
                });
            }
        } catch (...) {
            (void)cudaStreamSynchronize(device.transfer_stream);
            for_each_pending_pressure([&](auto& work) { abort_pressure_work(work); });
            throw;
        }
        bool copies_submitted = false;
        for_each_pending_pressure(
            [&](const auto& work) { copies_submitted = copies_submitted || work.submitted; });
        pressure_transition.phase = copies_submitted ? PressureTransitionPhase::CopiesInFlight
                                                     : PressureTransitionPhase::CopyPublication;
        if (copies_submitted) {
            context_completion_.record(device.transfer_stream);
            return ActiveCaptureResult{.status = runtime::ContextTransactionStatus::InProgress};
        }
    }

    if (pressure_transition.phase == PressureTransitionPhase::CopiesInFlight) {
        if (!context_completion_.ready()) {
            return ActiveCaptureResult{.status = runtime::ContextTransactionStatus::InProgress};
        }
        pressure_transition.phase = PressureTransitionPhase::CopyPublication;
    }
    if (has_pressure() && pressure_transition.phase == PressureTransitionPhase::CopyPublication) {
        for (std::size_t position = 0; position < transaction.shared_pressure.size(); ++position) {
            auto& work = transaction.shared_pressure[position];
            if (!work.completed) {
                publish_pressure_work(work);
                collect_spill(work);
                transaction.shared_pressure_results[position] = MaterializationSharedVictimResult{
                    .owner              = transaction.shared_pressure_results[position].owner,
                    .disposition        = runtime::VictimDisposition::Retained,
                    .pressure_committed = true,
                    .final_summary      = shared_prefix_summary(
                        shared_prefix_states[transaction.shared_victim_indices[position]]),
                };
            }
        }
        for (std::size_t position = 0; position < transaction.pressure.size(); ++position) {
            auto& work = transaction.pressure[position];
            if (!work.completed) {
                publish_pressure_work(work);
                collect_spill(work);
                transaction.pressure_results[position] = MaterializationVictimResult{
                    .owner              = transaction.pressure_results[position].owner,
                    .disposition        = runtime::VictimDisposition::Retained,
                    .pressure_committed = true,
                    .final_summary      = continuation_summary(
                        continuation_states[transaction.victim_indices[position]]),
                };
            }
        }
        constexpr std::array resources{
            runtime::ContextResourceClass::State,
            runtime::ContextResourceClass::MainKV,
            runtime::ContextResourceClass::BackendKV,
        };
        for (const runtime::ContextResourceClass resource : resources) {
            const std::size_t index = context_resource_index(resource);
            if ((pressure_transition.timer_mask & (1U << index)) == 0) { continue; }
            transaction.transfer_observations.push_back(context_transfer_observation(
                resource, runtime::ContextTransferDirection::DeviceToHost,
                pressure_transition.transfer_work[index], pressure_transition.transfer_pages[index],
                pressure_transition.state_images));
        }
        pressure_transition.timer_mask = 0;
        pressure_transition.phase      = PressureTransitionPhase::Committed;
    }
    if (has_pressure() && pressure_transition.phase != PressureTransitionPhase::Committed) {
        throw std::logic_error("capture pressure transition did not reach a stable phase");
    }
    if (cancellation.requested()) { return abort(); }
    if (!transaction.prepared) {
        if (cancellation.requested()) { return abort(); }
        try {
            prepare_active_capture(transaction);
        } catch (...) {
            abort_active_capture(transaction);
            transaction.published = true;
            throw;
        }
    }
    if (transaction.transfer_enqueue_pending) {
        if (cancellation.requested()) { return abort(); }
        try {
            enqueue_active_capture_transfers(transaction);
        } catch (...) {
            if (device.transfer_stream != nullptr) {
                (void)cudaStreamSynchronize(device.transfer_stream);
            }
            abort_active_capture(transaction);
            transaction.published = true;
            throw;
        }
        // Fault injection (NINFER_INJECT_THROW=capture-submitted-host): the same instant as
        // `capture-submitted`, restricted to a HOST-SNAPSHOT capture -- the only placement that reaches
        // `abort_active_capture`'s `abort-snapshot-destination` release, a NON-STRICT release whose
        // refusal is exactly the #9 leak's shape (the handle is dropped while its host state slot and
        // host KV stay allocated). `capture-submitted` cannot target it: in the host-snapshot scenario
        // the occupying lane's earlier DeviceFork captures consume the injections first -- observed
        // 2026-09-26, where the plain site fired on lane 0 (the occupant) and the host-snapshot capture
        // on lane 1 was never aborted.
        if (transaction.state_placement == qwen3_5::CaptureStatePlacement::HostSnapshot &&
            ninfer::harmful_inject_throw("capture-submitted-host")) {
            throw std::bad_alloc();
        }
        // Fault injection (NINFER_INJECT_THROW=capture-submitted, harmful-controls build). Placed AFTER
        // the enqueue succeeded, so `transfer_submitted` is true and the fork's copy into the recycled
        // rewrite-checkpoint slot has been issued -- the precondition `abort_active_capture`'s recycling
        // branch was written for. Throwing here unwinds to the engine, whose failure path runs
        // `abort_active_capture` with that state in flight.
        //
        // That branch is unreachable by construction (plan.md §2 item 3: the planner strips
        // `group.rewrite` unless the disposition is `ReplaceAtCommittedFrontier`, and every `Replace`
        // activation clears the checkpoint first), and its epoch restore was replaced by a DROP when the
        // disposal was implemented. (`restore_recycled_checkpoint` still exists in the store: it is a
        // primitive with its own test, and only its use here was removed.) The injection is
        // kept because it is the control for the reachability claim: the probe in `inspect_capture`
        // aborts if `recycles_private_state` is ever true, and this is the site that would run next.
        // Probe before throwing, not after the abort: the first version of this probe sat inside
        // `abort_active_capture`, *after* that function's state restoration had already released the
        // rewrite checkpoint -- so it reported `rewrite_state_live=0` for a value it had destroyed the
        // ability to observe, and three windows were spent on a reading that could not vary. Here the
        // five conditions `recycles_private_state` is built from (capture.cpp:110-112) are measured as
        // the abort will find them.
        if (std::getenv("NINFER_ABORT_PROBE") != nullptr &&
            transaction.lane < max_concurrency &&
            active_continuations[transaction.lane] < continuation_capacity) {
            SequenceState& probe_sequence = active_sequence(transaction.lane);
            const bool state_live =
                probe_sequence.rewrite_state.has_value() &&
                state_store->valid(*probe_sequence.rewrite_state);
            std::fprintf(stderr,
                         "[capture] pre-abort probe lane=%u recycling=%d rewrite_group=%d "
                         "rewrite_state_live=%d checkpoint_valid=%d slot_differs=%d can_recycle=%d "
                         "transfer_submitted=%d\n",
                         transaction.lane, static_cast<int>(transaction.recycles_private_state),
                         static_cast<int>(transaction.group.rewrite.has_value()),
                         static_cast<int>(state_live),
                         static_cast<int>(probe_sequence.rewrite_checkpoint.valid),
                         static_cast<int>(state_live &&
                                          *probe_sequence.rewrite_state !=
                                              probe_sequence.state.write),
                         static_cast<int>(state_live &&
                                          state_store->can_recycle_checkpoint_destination(
                                              *probe_sequence.rewrite_state)),
                         static_cast<int>(transaction.transfer_submitted));
            std::fflush(stderr);
        }
        if (ninfer::harmful_inject_throw("capture-submitted")) {
            throw std::bad_alloc();
        }
        return ActiveCaptureResult{.status = runtime::ContextTransactionStatus::InProgress};
    }
    if (transaction.transfer_submitted && !context_completion_.ready()) {
        return ActiveCaptureResult{.status = runtime::ContextTransactionStatus::InProgress};
    }
    if (transaction.transfer_submitted) {
        const auto record = [&](runtime::ContextResourceClass resource,
                                runtime::ContextTransferDirection direction, TransferWork work,
                                std::uint32_t pages) {
            const std::uint8_t bit =
                static_cast<std::uint8_t>(1U << context_resource_index(resource));
            if ((transaction.transfer_timer_mask & bit) == 0) { return; }
            transaction.transfer_observations.push_back(
                context_transfer_observation(resource, direction, work, pages));
            transaction.transfer_timer_mask &= static_cast<std::uint8_t>(~bit);
        };
        const auto planned_work = [&](runtime::ContextResourceClass resource,
                                      runtime::ContextTransferDirection direction) {
            const auto found = std::find_if(
                transaction.transfer_requirements.begin(), transaction.transfer_requirements.end(),
                [&](const auto& requirement) {
                    return requirement.resource == resource && requirement.direction == direction;
                });
            return found == transaction.transfer_requirements.end() ? TransferWork{} : found->work;
        };
        const runtime::ContextTransferDirection state_direction =
            transaction.state_placement == qwen3_5::CaptureStatePlacement::HostSnapshot
                ? runtime::ContextTransferDirection::DeviceToHost
                : runtime::ContextTransferDirection::DeviceToDevice;
        record(runtime::ContextResourceClass::State, state_direction,
               planned_work(runtime::ContextResourceClass::State, state_direction), 0);
        record(runtime::ContextResourceClass::MainKV,
               runtime::ContextTransferDirection::DeviceToDevice,
               planned_work(runtime::ContextResourceClass::MainKV,
                            runtime::ContextTransferDirection::DeviceToDevice),
               1);
        if (backend_kv_pages) {
            record(runtime::ContextResourceClass::BackendKV,
                   runtime::ContextTransferDirection::DeviceToDevice,
                   planned_work(runtime::ContextResourceClass::BackendKV,
                                runtime::ContextTransferDirection::DeviceToDevice),
                   1);
        }
        transaction.transfer_submitted = false;
    }
    if (cancellation.requested()) { return abort(); }
    return publish_active_capture(transaction);
}


} // namespace ninfer::models::qwen3_5::detail
