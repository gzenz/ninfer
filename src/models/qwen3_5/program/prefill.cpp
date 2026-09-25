#include "models/qwen3_5/program/program_impl.h"
#include "models/qwen3_5/program/context_work.h"
#include "models/qwen3_5/program/context.h"
#include "models/qwen3_5/execution/linear.h"
#include "core/device.h"
#include "core/diagnostics.h"
#include "ninfer/ops/gdn_replay.h"
#include "ninfer/ops/sampling.h"
#include "ninfer/ops/scalar.h"
#include "ninfer/ops/scatter.h"
#include "ninfer/ops/speculative_round.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace ninfer::models::qwen3_5::execution {
namespace {

DFlashFeatureSink make_dflash_prefill_sink(PrefillContext& state) {
    if (!state.execution.io.dflash_decode || state.dflash_host_ingress == nullptr) {
        throw std::logic_error("DFlash prefill controls are unavailable");
    }
    return dflash_feature_sink(
        state, [&state](const Tensor& features, const Tensor& positions, bool rewrite_checkpoint) {
            auto& frame  = *state.execution.io.dflash_decode;
            Tensor count = frame.append_counts.slice(0, 0, 1);
            Tensor lane  = frame.state_destination_slots.slice(0, 0, 1);
            Tensor row   = frame.dflash_kv_table_rows.slice(0, 0, 1);
            ops::set_i32_scalar(count, features.ne[1], state.execution.device.stream);
            const auto exact = static_cast<std::uint32_t>(features.ne[1]);
            dflash_append_context(state, features, positions, count, lane, row, {exact, exact});
            (void)rewrite_checkpoint;
        });
}

} // namespace

void configure_text_card(TextContext& card, const ExecutionCore& execution,
                         const ops::SamplingConfig* sampling, std::int32_t state_source_slot,
                         std::int32_t state_destination_slot, std::uint32_t mtp_proposal_extent) {
    card.set_sampling(sampling);
    card.set_linear_state_slots(state_source_slot, state_destination_slot);
    card.set_gdn_state_action(GdnStateAction::UpdateInPlace, nullptr);
    card.set_mtp_proposal_extent(mtp_proposal_extent);
    card.set_rope_scaling(execution.rope_scaling_factor, execution.rope_scaling_original_context);
    if (execution.proposal_head == ProposalHead::Full) {
        card.set_proposal_head(nullptr, nullptr, 0);
        return;
    }
    if (card.proposal_head() == nullptr || card.proposal_head_n() <= 0) {
        throw std::runtime_error("optimized proposal head is unavailable");
    }
}

PrefillChunkResult prefill_text_chunk(PrefillContext& state, std::span<const TokenId> ids,
                                      std::uint32_t nominal_length,
                                      std::optional<std::uint32_t> split_frontier,
                                      bool finalize_at_end) {
    TextContext card(state.execution.device, state.execution.parameters, state.execution.work,
                     state.text_kv, state.execution.linear_attention, state.execution.io,
                     state.execution.prefill_hidden, state.execution.prefill_chunk,
                     state.text_kv_base, state.mtp_kv, &state.text_cache, state.mtp_cache);
    configure_text_card(card, state.execution, state.sampling, state.state_source_slot,
                        state.state_destination_slot, state.mtp_proposal_extent);
    card.set_rewrite_checkpoint_hidden_output(state.rewrite_checkpoint_hidden);
    card.set_prefill_split_frontier(split_frontier ? static_cast<std::int64_t>(*split_frontier)
                                                   : -1);
    const std::span<const int> prompt(ids.data(), ids.size());
    if (state.dflash != nullptr) {
        DFlashFeatureSink sink = make_dflash_prefill_sink(state);
        return card.prefill_chunk(prompt, state.text_kv_base, nominal_length, finalize_at_end,
                                  sink);
    }
    return card.prefill_chunk(prompt, state.text_kv_base, nominal_length, finalize_at_end);
}

PrefillChunkResult prefill_multimodal_chunk(PrefillContext& state, const PreparedPromptData& prompt,
                                            VisionPrefillSession& vision,
                                            std::uint32_t nominal_length,
                                            std::optional<std::uint32_t> split_frontier,
                                            bool finalize_at_end) {
    TextContext card(state.execution.device, state.execution.parameters, state.execution.work,
                     state.text_kv, state.execution.linear_attention, state.execution.io,
                     state.execution.prefill_hidden, state.execution.prefill_chunk,
                     state.text_kv_base, state.mtp_kv, &state.text_cache, state.mtp_cache);
    configure_text_card(card, state.execution, state.sampling, state.state_source_slot,
                        state.state_destination_slot, state.mtp_proposal_extent);
    card.set_rewrite_checkpoint_hidden_output(state.rewrite_checkpoint_hidden);
    card.set_prefill_split_frontier(split_frontier ? static_cast<std::int64_t>(*split_frontier)
                                                   : -1);
    if (state.dflash != nullptr) {
        DFlashFeatureSink sink = make_dflash_prefill_sink(state);
        return card.prefill_chunk(prompt, state.text_kv_base, nominal_length, vision,
                                  finalize_at_end, sink);
    }
    return card.prefill_chunk(prompt, state.text_kv_base, nominal_length, vision, finalize_at_end);
}

void mtp_bridge_multimodal(PrefillContext& state, const PreparedPromptData& prompt,
                           VisionPrefillSession& vision, const MtpBridgeInput& bridge) {
    if (!state.mtp_kv.valid() || bridge.previous_hidden == nullptr || state.text_kv_base == 0 ||
        bridge.position < 0 ||
        static_cast<std::uint32_t>(bridge.position) + 1 != state.text_kv_base) {
        throw std::logic_error("multimodal MTP bridge does not match the reusable frontier");
    }

    Tensor bridge_token = state.execution.io.mtp->target_input_ids.slice(0, 0, 1);
    const TokenId token = prompt.token_ids[state.text_kv_base];
    // Async + settle: stack-local source, non-blocking compute stream -- keep ordering, then wait.
    CUDA_CHECK(cudaMemcpyAsync(bridge_token.data, &token, sizeof(token), cudaMemcpyHostToDevice,
                               state.execution.device.stream));
    CUDA_CHECK(cudaStreamSynchronize(state.execution.device.stream));

    Tensor visual_embedding;
    const Tensor* composed_embedding = nullptr;
    if (prompt.token_types[state.text_kv_base] != 0) {
        const VisionChunk chunk = vision.prepare_chunk(state.text_kv_base, 1);
        if (chunk.control == nullptr) {
            throw std::logic_error("visual MTP bridge has no encoded Vision item");
        }
        const auto& scatter = chunk.control->scatter_indices;
        const auto column   = std::lower_bound(scatter.begin(), scatter.end(),
                                               static_cast<std::int32_t>(state.text_kv_base));
        if (column == scatter.end() || *column != static_cast<std::int32_t>(state.text_kv_base) ||
            static_cast<std::uint8_t>(chunk.control->modality) !=
                prompt.token_types[state.text_kv_base]) {
            throw std::logic_error("visual MTP bridge does not match Vision scatter metadata");
        }
        visual_embedding =
            chunk.embeddings.slice(1, static_cast<std::int32_t>(column - scatter.begin()), 1);
        composed_embedding = &visual_embedding;
    }

    mtp_bridge_and_propose(state, bridge_token, *bridge.previous_hidden, bridge.position,
                           bridge.rope_position, false, composed_embedding);
}

void sample_from_hidden(PrefillContext& state, const Tensor& hidden, std::int32_t absolute_position,
                        std::int32_t purpose) {
    if (hidden.dtype != DType::BF16 ||
        hidden.ne[0] != dimension(state.execution.parameters.model.config().text.hidden_size) ||
        hidden.ne[1] != 1 || hidden.ne[2] != 1 || hidden.ne[3] != 1 || hidden.data == nullptr) {
        throw std::invalid_argument("sample_from_hidden requires BF16 [hidden,1]");
    }
    state.execution.work.reset();
    Tensor logits = state.execution.io.logits.slice(1, 0, 1);
    project(hidden, state.execution.parameters.text.output_head, logits, state.execution.work,
            state.execution.device.stream);
    // Async + settle: `absolute_position` is a parameter and is the sampler's logical position
    // (its RNG key); keep the copy ordered on the non-blocking stream, then wait for it.
    CUDA_CHECK(cudaMemcpyAsync(state.execution.io.pos.data, &absolute_position,
                               sizeof(absolute_position), cudaMemcpyHostToDevice,
                               state.execution.device.stream));
    CUDA_CHECK(cudaStreamSynchronize(state.execution.device.stream));
    ops::sample(logits, state.execution.io.token,
                dimension(state.execution.parameters.model.resources().public_token_count),
                state.sampling, state.execution.io.pos, purpose, state.execution.work,
                state.execution.device.stream);
    state.execution.work.reset();
}

} // namespace ninfer::models::qwen3_5::execution

namespace ninfer::models::qwen3_5::detail {

namespace {

std::array<std::int32_t, 3> prompt_rope_position(const PreparedPromptData& prompt,
                                                 std::uint32_t token);

std::array<std::int32_t, 3> prompt_rope_position(const PreparedPromptData& prompt,
                                                 std::uint32_t token) {
    const std::size_t tokens = prompt.token_ids.size();
    if (token >= tokens || prompt.positions.size() != 3 * tokens) {
        throw std::invalid_argument("MTP bridge position is outside prepared prompt metadata");
    }
    return {prompt.positions[token], prompt.positions[tokens + token],
            prompt.positions[2 * tokens + token]};
}

// Host-side mirror of ops::scale_positions_yarn (src/ops/kernel/position.cuh). Applied to the
// host-written RoPE position values that enter the target model (the staged-prefill completion
// and bridge values here; the spec-decode target ingress lives in decode.cpp).
[[nodiscard]] std::int32_t yarn_scale_position(std::int32_t position, std::uint32_t original_context,
                                               float factor) noexcept {
    if (factor == 1.0F || position <= static_cast<std::int32_t>(original_context)) {
        return position;
    }
    const float scaled = static_cast<float>(original_context) +
                         (static_cast<float>(position - static_cast<std::int32_t>(original_context))) /
                             factor +
                         0.5F;
    return static_cast<std::int32_t>(scaled);
}

} // namespace

void ProgramImpl::bind_dflash_prefill_sink(SequenceState& sequence) {
    // One derivation, called from every prefill step and from materialization. Two call sites were
    // how this drifted before: the materialization site re-derived the values, the multi-step path
    // did not, and the comment claimed both did.
    if (!is_masked_draft_backend(speculative_backend) || dflash_host_ingress == nullptr ||
        !io.dflash_decode.has_value()) {
        return;
    }
    const StateImageSelectors selectors = state_selectors(sequence);
    *dflash_host_ingress                            = {};
    dflash_host_ingress->active_lanes[0]            = static_cast<std::int32_t>(sequence.lane);
    dflash_host_ingress->state_source_slots[0]      = selectors.source;
    dflash_host_ingress->state_destination_slots[0] = selectors.destination;
    dflash_host_ingress->dflash_kv_table_rows[0] =
        (sequence.kv && sequence.kv->backend)
            ? backend_kv_addresses->bound_row(*sequence.kv->backend)
            : 0;
    CUDA_CHECK(cudaMemcpyAsync(io.dflash_decode->ingress.data, dflash_host_ingress,
                               sizeof(qwen3_5::DFlashDecodeIngress), cudaMemcpyHostToDevice,
                               device.stream));
}

void ProgramImpl::start_sequence(std::uint32_t lane, SequenceState& sequence,
                                 MaterializationTransaction& transaction) {
    if (lane >= max_concurrency) { throw std::out_of_range("request lane is out of range"); }
    RequestControl& request = requests[lane];
    if (!transaction.plan || transaction.plan->impl_ == nullptr || !transaction.prepared ||
        !request.prefill) {
        throw std::invalid_argument("materialization staging is incomplete");
    }
    AdmissionCandidateImpl& request_plan = *transaction.plan->impl_;
    if (request.lifecycle == Lifecycle::Prefilling || request.lifecycle == Lifecycle::Active ||
        request.lifecycle == Lifecycle::Pending) {
        throw std::logic_error("staged prefill requires a free request lane");
    }
    auto& staged                           = *request.prefill;
    const auto started                     = Clock::now();
    const std::uint32_t prompt_tokens      = staged.prompt_tokens;
    const std::uint32_t base               = staged.base;
    const std::uint32_t initial_mtp_extent = staged.initial_mtp_extent;
    // Durable owner binding: the continuation keeps this value after the lane is released, so a
    // later adoption can tell whose checkpoint it is taking (W1.2).
    if (staged.prompt.context_cache.session_key) {
        std::uint64_t owner = 1469598103934665603ULL;
        for (const char byte : staged.prompt.context_cache.session_key->view()) {
            owner ^= static_cast<std::uint8_t>(byte);
            owner *= 1099511628211ULL;
        }
        sequence.session_key_hash = owner == 0 ? 1 : owner;
    } else {
        sequence.session_key_hash = 0;
    }
    sequence.admitted_prompt_tokens = prompt_tokens;
    // (removed) NINFER_PROMPT_PROBE / PROMPT-FOREIGN-DOC: it scanned every stored session prompt
    // against every new prompt (4 x 32k x 32k x 64 comparisons on the admission path) and retained
    // every prompt for the life of the process. Its question is answered -- no lane's prompt carries
    // another session's document -- and an instrument that can hang or leak on the served path does
    // not belong in the tree. Re-implement offline over captured request payloads if it is ever
    // needed again.
    request.lifecycle                      = Lifecycle::Empty;
    try {
        const std::uint32_t state_slots = request_plan.demand.active_entitlement.device.state_slots;
        const bool preserving_source =
            (transaction.has_source || transaction.has_shared_source) &&
            transaction.source_mode == runtime::PrivateSourceMode::Retain;
        const bool text_prefix_fork    = request_plan.text_prefix_fork_required;
        const bool backend_prefix_fork = request_plan.backend_prefix_fork_required;
        if (request_plan.reuse == ReusePath::Root) {
            if (transaction.reserved_state_count != state_slots || state_slots == 0 ||
                !transaction.root_text_address || !transaction.text_activation ||
                transaction.root_backend_address.has_value() !=
                    (request_plan.backend_kv_page_entitlement != 0) ||
                transaction.backend_activation.has_value() !=
                    (request_plan.backend_kv_page_entitlement != 0)) {
                throw std::logic_error("root materialization reservations are incomplete");
            }
            release_sequence_kv(sequence);
            release_sequence_state(sequence);
            sequence.state = ActiveStateBinding{.read  = transaction.reserved_states[0],
                                                .write = transaction.reserved_states[0]};
            transaction.reserved_states[0] = {};
            if (state_slots == 2) {
                sequence.reserved_state        = transaction.reserved_states[1];
                transaction.reserved_states[1] = {};
            }
            transaction.reserved_state_count = 0;

            SequenceKVBundle bundle{.text = *transaction.root_text_address};
            transaction.root_text_address.reset();
            if (transaction.root_backend_address) {
                bundle.backend = *transaction.root_backend_address;
                transaction.root_backend_address.reset();
            }
            sequence.kv.emplace(bundle);
        } else if (preserving_source) {
            const bool private_source_ready = transaction.has_source &&
                                              transaction.source_index < continuation_capacity &&
                                              continuation_slots[transaction.source_index].role ==
                                                  ContinuationSlotRole::Catalogued;
            const bool shared_source_ready =
                transaction.has_shared_source &&
                transaction.shared_source_index < shared_prefix_capacity &&
                shared_prefix_slots[transaction.shared_source_index].role ==
                    SharedPrefixSlotRole::Catalogued;
            if (private_source_ready == shared_source_ready ||
                transaction.reserved_state_count != state_slots || state_slots == 0 ||
                !transaction.root_text_address || !transaction.text_prefix_fork ||
                !transaction.prefix_forks_ready ||
                transaction.root_backend_address.has_value() !=
                    (request_plan.backend_kv_page_entitlement != 0)) {
                throw std::logic_error("retained materialization is incomplete");
            }
            const StateImageHandle selected =
                private_source_ready
                    ? selected_state(continuation_states[transaction.source_index],
                                     request_plan.reuse, request_plan.selected_checkpoint)
                    : shared_prefix_states[transaction.shared_source_index].state;
            const StateImageHandle current = transaction.reserved_states[0];
            if (state_store->residency(selected) == StateReplicaResidency::HostOnly) {
                if (state_store->role(current) != StateImageRole::ActiveMutable) {
                    throw std::logic_error("Host retained Fork destination was not published");
                }
                sequence.state = ActiveStateBinding{.read = current, .write = current};
            } else if (transaction.split_state_identity) {
                if (!private_source_ready ||
                    state_store->residency(selected) != StateReplicaResidency::Both) {
                    throw std::logic_error("StateImage identity split source changed");
                }
                state_store->split_device_replica_identity(selected, current);
                sequence.state = ActiveStateBinding{.read = current, .write = current};
            } else {
                // Fork-side determinism control (NINFER_FORK_ZERO=1): a fork destination is
                // filled by the ops that consume the selector pair, so any region they do not
                // cover keeps the previous occupant's bytes -- another sequence's state. Root
                // admissions zero their slot (activate_reset); forks do not. Zeroing here is a
                // behavioural control that either removes the cross-session bleed (stale fork
                // destination bytes are the carrier) or leaves it untouched. Compiled out unless
                // the build enables harmful controls (see diagnostic_control_enabled).
                if (diagnostic_control_enabled("NINFER_FORK_ZERO")) {
                    state_store->zero_reserved_device_replica(current, device.stream);
                }
                const StateImageSelectors selectors = state_store->begin_fork(selected, current);
                // Fork completeness control (NINFER_FORK_COPY=1): initialize the whole
                // destination image from the source before the ops run, so a region the ops
                // read but never write cannot carry the previous occupant's state. The zeroing
                // control above changed behaviour (a lane returned an empty reply), which says
                // some region IS read before it is written.
                if (diagnostic_control_enabled("NINFER_FORK_COPY")) {
                    state_images->copy_slot(selectors.source, selectors.destination, device.stream);
                }
                if (is_masked_draft_backend(speculative_backend)) {
                    state_images->copy_dflash_local(selectors.source, selectors.destination,
                                                    device.stream);
                }
                sequence.state = ActiveStateBinding{
                    .read           = selected,
                    .write          = current,
                    .fork_pending   = true,
                    .read_ownership = StateReadOwnership::ExternalOwner,
                };
            }
            transaction.reserved_states[0]   = {};
            transaction.split_state_identity = false;
            if (state_slots == 2) {
                sequence.reserved_state        = transaction.reserved_states[1];
                transaction.reserved_states[1] = {};
            }
            transaction.reserved_state_count = 0;
            sequence.rewrite_state.reset();
            sequence.rewrite_checkpoint = {};

            SequenceKVBundle bundle{.text = *transaction.root_text_address};
            transaction.root_text_address.reset();
            if (transaction.root_backend_address) {
                bundle.backend = *transaction.root_backend_address;
                transaction.root_backend_address.reset();
            }
            sequence.kv.emplace(bundle);
        } else {
            if (request_plan.state_fork_required !=
                transaction.state_fork_destination.has_value()) {
                throw std::logic_error("private materialization StateImage Fork is incomplete");
            }
            if (transaction.reserved_state_count > 1 ||
                (transaction.reserved_state_count != 0 && sequence.reserved_state)) {
                throw std::logic_error("private materialization StateImage reservation is invalid");
            }
            if (transaction.reserved_state_count == 1) {
                sequence.reserved_state          = transaction.reserved_states[0];
                transaction.reserved_states[0]   = {};
                transaction.reserved_state_count = 0;
            }
        }

        if (!preserving_source) {
            std::array<HostKVPageReplicaRelease, 2> stale_tail_replicas{};
            std::size_t stale_tail_count           = 0;
            const auto preflight_inactive_truncate = [&](KVAddressSpaceStore& addresses,
                                                         LogicalKVPageStore& pages,
                                                         KVAddressSpaceHandle address,
                                                         std::optional<std::uint32_t> frontier) {
                if (!frontier ||
                    (addresses.committed_frontier(address) == *frontier &&
                     addresses.mapped_pages(address) == kv_pages_for_frontier(*frontier))) {
                    return;
                }
                bool releases_tail               = false;
                const std::uint32_t target_pages = kv_pages_for_frontier(*frontier);
                if (target_pages != 0) {
                    const LogicalKVPageHandle tail =
                        addresses.logical_page(address, target_pages - 1U);
                    const std::uint32_t columns =
                        *frontier -
                        (target_pages - 1U) * static_cast<std::uint32_t>(kPagedKVPageSize);
                    if (columns != pages.committed_columns(tail) && pages.host_resident(tail)) {
                        if (host_kv_extents == nullptr ||
                            stale_tail_count == stale_tail_replicas.size()) {
                            throw std::logic_error("stale Host KV tail replica is not releasable");
                        }
                        stale_tail_replicas[stale_tail_count++] =
                            HostKVPageReplicaRelease{.pages = &pages, .page = tail};
                        releases_tail = true;
                    }
                }
                if (!addresses.can_destructive_truncate_inactive(address, *frontier,
                                                                 releases_tail)) {
                    throw std::logic_error(
                        "selected private KV frontier is not destructively materializable");
                }
            };
            if (!sequence.kv) {
                throw std::logic_error("materialization destination has no KV address space");
            }
            if (!text_prefix_fork) {
                preflight_inactive_truncate(*text_kv_addresses, *text_kv_pages, sequence.kv->text,
                                            transaction.text_activation_frontier);
            }
            if (sequence.kv->backend && !backend_prefix_fork) {
                preflight_inactive_truncate(*backend_kv_addresses, *backend_kv_pages,
                                            *sequence.kv->backend,
                                            transaction.backend_activation_frontier);
            }
            if (stale_tail_count != 0) {
                const std::span<const HostKVPageReplicaRelease> releases(stale_tail_replicas.data(),
                                                                         stale_tail_count);
                if (!host_kv_extents->release_page_replicas(releases)) {
                    throw std::logic_error(
                        "stale Host KV tail replicas cannot be released atomically");
                }
            }
            if (!text_prefix_fork && transaction.text_activation_frontier &&
                (text_kv_addresses->committed_frontier(sequence.kv->text) !=
                     *transaction.text_activation_frontier ||
                 text_kv_addresses->mapped_pages(sequence.kv->text) !=
                     kv_pages_for_frontier(*transaction.text_activation_frontier))) {
                text_kv_addresses->destructive_truncate_inactive(
                    sequence.kv->text, *transaction.text_activation_frontier);
            }
            if (!backend_prefix_fork && transaction.backend_activation_frontier &&
                sequence.kv->backend &&
                (backend_kv_addresses->committed_frontier(*sequence.kv->backend) !=
                     *transaction.backend_activation_frontier ||
                 backend_kv_addresses->mapped_pages(*sequence.kv->backend) !=
                     kv_pages_for_frontier(*transaction.backend_activation_frontier))) {
                backend_kv_addresses->destructive_truncate_inactive(
                    *sequence.kv->backend, *transaction.backend_activation_frontier);
            }
            if (host_kv_extents) { (void)host_kv_extents->release_unreferenced(); }
        }
        if ((text_prefix_fork || backend_prefix_fork) && !transaction.prefix_forks_ready) {
            throw std::logic_error("materialization prefix forks are incomplete");
        }
        if (text_prefix_fork) {
            text_kv_addresses->commit_prefix_fork(std::move(*transaction.text_prefix_fork),
                                                  device.stream);
            transaction.text_prefix_fork.reset();
            if (!preserving_source) {
                const KVAddressSpaceHandle source_address = sequence.kv->text;
                sequence.kv->text                         = *transaction.root_text_address;
                transaction.root_text_address.reset();
                if (!text_kv_addresses->release(source_address)) {
                    throw std::logic_error("consumed Text KV source remained pinned after COW");
                }
            }
        } else {
            text_kv_addresses->commit_activation(std::move(*transaction.text_activation),
                                                 device.stream);
            transaction.text_activation.reset();
        }
        if (backend_prefix_fork) {
            backend_kv_addresses->commit_prefix_fork(std::move(*transaction.backend_prefix_fork),
                                                     device.stream);
            transaction.backend_prefix_fork.reset();
            if (!preserving_source) {
                const KVAddressSpaceHandle source_address = *sequence.kv->backend;
                sequence.kv->backend                      = *transaction.root_backend_address;
                transaction.root_backend_address.reset();
                if (!backend_kv_addresses->release(source_address)) {
                    throw std::logic_error("consumed Backend KV source remained pinned after COW");
                }
            }
        } else if (transaction.backend_activation) {
            backend_kv_addresses->commit_activation(std::move(*transaction.backend_activation),
                                                    device.stream);
            transaction.backend_activation.reset();
        }
        transaction.prefix_forks_ready = false;
        transaction.text_activation_frontier.reset();
        transaction.backend_activation_frontier.reset();
        transaction.prepared = false;

        const bool preserve_rewrite =
            request_plan.rewrite_disposition == RewriteCheckpointDisposition::RetainExisting;
        const auto activate_consumed_state = [&](StateImageHandle selected) {
            if (!request_plan.state_fork_required) {
                if (transaction.state_fork_destination ||
                    state_store->checkpoint_references(selected) != 0) {
                    throw std::logic_error("planned StateImage Move is no longer valid");
                }
                state_store->move_checkpoint_to_active(selected);
                sequence.state = ActiveStateBinding{.read = selected, .write = selected};
                return;
            }
            if (!transaction.state_fork_destination ||
                state_store->checkpoint_references(selected) == 0) {
                throw std::logic_error("planned StateImage Fork is no longer valid");
            }
            const StateImageHandle destination = *transaction.state_fork_destination;
            if (transaction.state_restored) {
                if (state_store->role(destination) != StateImageRole::ActiveMutable) {
                    throw std::logic_error("restored StateImage Fork destination is unavailable");
                }
                sequence.state = ActiveStateBinding{.read = destination, .write = destination};
            } else {
                const std::uint32_t references = state_store->checkpoint_references(selected);
                const std::uint32_t lineage_references =
                    owned_checkpoint_references(sequence, selected);
                if (lineage_references > references) {
                    throw std::logic_error("consumed StateImage Fork ownership is inconsistent");
                }
                const StateReadOwnership read_ownership =
                    lineage_references == references ? StateReadOwnership::LineageCheckpoint
                                                     : StateReadOwnership::ExternalOwner;
                if (diagnostic_control_enabled("NINFER_FORK_ZERO") &&
                    state_store->role(destination) == StateImageRole::ReservedDestination) {
                    state_store->zero_reserved_device_replica(destination, device.stream);
                }
                const StateImageSelectors selectors =
                    state_store->begin_fork(selected, destination);
                if (diagnostic_control_enabled("NINFER_FORK_COPY")) {
                    state_images->copy_slot(selectors.source, selectors.destination, device.stream);
                }
                if (is_masked_draft_backend(speculative_backend)) {
                    state_images->copy_dflash_local(selectors.source, selectors.destination,
                                                    device.stream);
                }
                sequence.state = ActiveStateBinding{
                    .read           = selected,
                    .write          = destination,
                    .fork_pending   = true,
                    .read_ownership = read_ownership,
                };
            }
            transaction.state_fork_destination.reset();
        };
        if (request_plan.reuse == ReusePath::Root) {
            sequence.rewrite_checkpoint = {};
            ordered_reset(sequence);
            sequence.ledger.clear();
            sequence.prefix_digests.clear();
            sequence.text_kv_valid = 0;
            sequence.mtp_kv_valid  = 0;
        } else if (preserving_source) {
            const SequenceState* private_source =
                transaction.has_source ? &continuation_states[transaction.source_index] : nullptr;
            SharedPrefixState* shared_source =
                transaction.has_shared_source
                    ? &shared_prefix_states[transaction.shared_source_index]
                    : nullptr;
            const std::uint32_t source_text_frontier =
                private_source != nullptr ? private_source->text_kv_valid : shared_source->frontier;
            if (!sequence.kv || source_text_frontier < base) {
                throw std::logic_error("retained prefix has incomplete Text KV");
            }
            sequence.text_kv_valid = base;
            if (speculative_backend == SpeculativeBackend::Mtp) {
                const std::uint32_t mtp_base       = base == 0 ? 0 : base - 1U;
                const std::uint32_t source_backend = private_source != nullptr
                                                         ? private_source->mtp_kv_valid
                                                         : shared_source->backend_frontier;
                if (!request_plan.prepare_mtp || source_backend < mtp_base) {
                    throw std::logic_error("retained prefix has incomplete MTP KV");
                }
                sequence.mtp_kv_valid = mtp_base;
            } else if (is_masked_draft_backend(speculative_backend)) {
                const std::uint32_t source_backend = private_source != nullptr
                                                         ? private_source->dflash_context_frontier
                                                         : shared_source->frontier;
                if (source_backend < base) {
                    throw std::logic_error("retained prefix has incomplete DFlash KV");
                }
                sequence.dflash_context_frontier = base;
            }
            sequence.tail_hidden_valid =
                base == prompt_tokens &&
                (private_source != nullptr ? private_source->tail_hidden_valid
                                           : shared_source->tail_hidden_valid);
            if (shared_source != nullptr) {
                if (shared_source->active_references == std::numeric_limits<std::uint32_t>::max()) {
                    throw std::overflow_error("shared-prefix active reference overflow");
                }
                ++shared_source->active_references;
                sequence.shared_prefix_references.push_back(transaction.shared_source_index);
            }
            refresh_state_views(sequence);
            bind_sequence_kv(sequence);
            // The DFlash sink has the same defect as the KV row above, and it matters on prod's
            // backend: the sink reads its destination slot and table row from
            // `io.dflash_decode->ingress`, a single device buffer that is uploaded once at
            // materialization and then overwritten by EVERY dflash decode round of ANY lane. A
            // prefill interleaved with other lanes' decode rounds therefore appended drafter context
            // into another lane's dflash KV and consumed another lane's state destination slot.
            //
            // 2026-09-25 correction: the 479c92c4 commit message claims this re-upload happens "at
            // each step" and this comment said the same. It did not: the block below is inside
            // `start_sequence`, so it ran once per materialization, and `advance_prefill` -- the
            // multi-step path a long prompt actually takes -- re-bound only the KV row scalars. The
            // shared helper is now called from both, which is what makes the claim true.
            bind_dflash_prefill_sink(sequence);
        } else if (request_plan.reuse == ReusePath::PrivateEndpoint) {
            if (!state_store->valid(sequence.state.read) ||
                sequence.state.read != sequence.state.write || sequence.state.fork_pending ||
                state_store->role(sequence.state.read) != StateImageRole::CheckpointImmutable) {
                throw std::logic_error("resident endpoint StateImage is not movable");
            }
            if (!preserve_rewrite && sequence.rewrite_state) {
                const StateImageHandle dropped = *sequence.rewrite_state;
                state_store->release_checkpoint_reference(dropped);
                sequence.rewrite_state.reset();
                sequence.rewrite_checkpoint = {};
                if (dropped != sequence.state.read &&
                    state_store->checkpoint_references(dropped) == 0 &&
                    !state_store->release(dropped)) {
                    throw std::logic_error("dropped rewrite StateImage could not be released");
                }
            }
            activate_consumed_state(sequence.state.read);
            if (!sequence.kv) {
                throw std::logic_error("resident prefix has no KV allocation bundle");
            }
            if (sequence.text_kv_valid < base) {
                throw std::logic_error("resident Text KV is shorter than the append frontier");
            }
            if (speculative_backend == SpeculativeBackend::Mtp) {
                const std::uint32_t mtp_base = base == 0 ? 0 : base - 1;
                if (!request_plan.prepare_mtp || sequence.mtp_kv_valid < mtp_base) {
                    throw std::logic_error("resident MTP KV is shorter than the bridge frontier");
                }
                sequence.mtp_kv_valid = mtp_base;
            } else if (is_masked_draft_backend(speculative_backend) &&
                       sequence.dflash_context_frontier != base) {
                throw std::logic_error("resident DFlash context is not at the append frontier");
            }
            bind_sequence_kv(sequence);
            trim_sequence_kv(sequence, base, backend_kv_valid(sequence));
            resize_sequence_kv_entitlement(sequence, request_plan.text_kv_page_entitlement,
                                           request_plan.backend_kv_page_entitlement);
            sequence.text_kv_valid = base;
            sequence.ledger.resize(base);
            sequence.prefix_digests.truncate(base);
            reserve_state_entitlement(sequence, state_slots);
            refresh_state_views(sequence);
        } else if (is_rewrite_checkpoint_restore(request_plan.reuse)) {
            if (!sequence.kv || sequence.text_kv_valid < base) {
                throw std::logic_error("resident rewrite checkpoint has no complete KV allocation");
            }
            if (!sequence.rewrite_state || !state_store->valid(*sequence.rewrite_state) ||
                state_store->role(*sequence.rewrite_state) != StateImageRole::CheckpointImmutable ||
                (sequence.endpoint_valid &&
                 (!state_store->valid(sequence.state.read) ||
                  sequence.state.read != sequence.state.write || sequence.state.fork_pending ||
                  state_store->role(sequence.state.read) != StateImageRole::CheckpointImmutable))) {
                throw std::logic_error("resident rewrite StateImage is not movable");
            }
            const StateImageHandle checkpoint = *sequence.rewrite_state;
            if (sequence.endpoint_valid && sequence.state.read == checkpoint) {
                throw std::logic_error("resident endpoint aliases its rewrite StateImage");
            }
            if (sequence.endpoint_valid && !state_store->release(sequence.state.read)) {
                throw std::logic_error("superseded endpoint StateImage could not be released");
            }
            if (!preserve_rewrite) {
                state_store->release_checkpoint_reference(checkpoint);
                sequence.rewrite_state.reset();
                sequence.rewrite_checkpoint = {};
            }
            activate_consumed_state(checkpoint);
            sequence.text_kv_valid = base;
            if (speculative_backend == SpeculativeBackend::Mtp) {
                const std::uint32_t mtp_base = base == 0 ? 0 : base - 1;
                if (!request_plan.prepare_mtp || sequence.mtp_kv_valid < mtp_base) {
                    throw std::logic_error(
                        "rewrite-checkpoint MTP KV is shorter than the bridge frontier");
                }
                sequence.mtp_kv_valid = mtp_base;
            } else if (is_masked_draft_backend(speculative_backend)) {
                if (!dflash || (backend_kv_cache() && !sequence.kv->backend) ||
                    sequence.dflash_context_frontier < base) {
                    throw std::logic_error("planned DFlash rewrite checkpoint is unavailable");
                }
                sequence.dflash_context_frontier = base;
            }
            bind_sequence_kv(sequence);
            trim_sequence_kv(sequence, base, backend_kv_valid(sequence));
            resize_sequence_kv_entitlement(sequence, request_plan.text_kv_page_entitlement,
                                           request_plan.backend_kv_page_entitlement);
            sequence.tail_hidden_valid = base == prompt_tokens;
            sequence.ledger.resize(base);
            sequence.prefix_digests.truncate(base);
            reserve_state_entitlement(sequence, state_slots);
            refresh_state_views(sequence);
        } else {
            throw std::logic_error("request plan has an invalid prefix reuse path");
        }

        sequence.endpoint_valid = false;
        if (!preserving_source) { trim_sequence_kv(sequence, base, backend_kv_valid(sequence)); }
        bind_sequence_kv(sequence);
        const std::uint32_t backend_materialized =
            speculative_backend == SpeculativeBackend::Mtp
                ? std::min(capacity,
                           prompt_tokens + (initial_mtp_extent == 0 ? 0U : initial_mtp_extent - 1U))
            : speculative_backend == SpeculativeBackend::DFlash ? prompt_tokens
                                                                : 0U;
        ensure_sequence_kv_mapped(sequence, prompt_tokens, backend_materialized);
        // Prefill-side write audit (NINFER_MAT_DEBUG=1): everything else is cleared, so print
        // what each lane binds and writes at prefill activation -- the KV table rows (text and
        // backend), the state slots and the token range. Two lanes naming the same row or slot
        // in one window is the prefill-side aliasing signature.
        if (std::getenv("NINFER_MAT_DEBUG") != nullptr) {
            // Occupancy first: a one-line census otherwise reads as "no concurrency" while it is
            // really "every other lane was empty". `lifecycles` uses one character per lane:
            // E=Empty, P=Prefilling, A=Active, and . for any other state.
            std::string lifecycles(max_concurrency, '.');
            std::uint32_t occupied = 0;
            for (std::uint32_t other = 0; other < max_concurrency; ++other) {
                switch (requests[other].lifecycle) {
                case Lifecycle::Empty: lifecycles[other] = 'E'; break;
                case Lifecycle::Prefilling: lifecycles[other] = 'P'; ++occupied; break;
                case Lifecycle::Active: lifecycles[other] = 'A'; ++occupied; break;
                default: ++occupied; break;
                }
            }
            std::fprintf(stderr, "[mat-debug] PREFILL-CENSUS active=%u lifecycles=%s\n", occupied,
                         lifecycles.c_str());
            // The *local* sequence, i.e. the object this step will actually use. `active_sequence`
            // may still resolve a lane to its previous continuation at the first step after
            // admission, so the per-lane census alone cannot show a reusing lane's binding.
            try {
                const StateImageSelectors local_selectors = state_selectors(sequence);
                std::fprintf(stderr,
                             "[mat-debug] PREFILL-LOCAL lane=%u base=%u cursor=%u prompt=%u "
                             "frontier=%u src_slot=%d dst_slot=%d fork_pending=%d reuse=%d "
                             "mapped=%u\n",
                             sequence.lane, requests[sequence.lane].prefill
                                                ? requests[sequence.lane].prefill->base
                                                : 0U,
                             requests[sequence.lane].prefill
                                 ? requests[sequence.lane].prefill->cursor
                                 : 0U,
                             requests[sequence.lane].prefill
                                 ? requests[sequence.lane].prefill->prompt_tokens
                                 : 0U,
                             sequence.execution_frontier, local_selectors.source,
                             local_selectors.destination, sequence.state.fork_pending ? 1 : 0,
                             requests[sequence.lane].prefill
                                 ? static_cast<int>(requests[sequence.lane].prefill->reuse)
                                 : -1,
                             sequence.kv ? text_kv_addresses->mapped_pages(sequence.kv->text) : 0U);
                std::fflush(stderr);
            } catch (const std::exception& skip) {
                std::fprintf(stderr, "[mat-debug] PREFILL-LOCAL-SKIP lane=%u reason=%s\n",
                             sequence.lane, skip.what());
                std::fflush(stderr);
            }
            // Simultaneous census of every active lane (one print, so the snapshot is atomic --
            // an interleaved per-lane print cannot prove simultaneity). For each lane: the KV
            // table row, the physical page id of its first and last mapped page, the state
            // slots it reads/writes. Two lanes naming the same row, page or slot outside a
            // legitimately shared prefix is the prefill-side aliasing signature.
            for (std::uint32_t other = 0; other < max_concurrency; ++other) {
                if (requests[other].lifecycle == Lifecycle::Empty) { continue; }
                // Instrumentation must never disturb the served path: a lane in a transient
                // state simply drops out of the census.
                try {
                const SequenceState& lane_state = active_sequence(other);
                if (!lane_state.kv) {
                    std::fprintf(stderr, "[mat-debug] PREFILL-CENSUS-SKIP lane=%u reason=no-kv\n",
                                 other);
                    continue;
                }
                if (!text_kv_addresses->valid(lane_state.kv->text)) {
                    std::fprintf(stderr,
                                 "[mat-debug] PREFILL-CENSUS-SKIP lane=%u reason=kv-not-occupied\n",
                                 other);
                    continue;
                }
                const std::uint32_t mapped_pages =
                    text_kv_addresses->mapped_pages(lane_state.kv->text);
                const std::uint64_t last_page_epoch =
                    mapped_pages == 0
                        ? 0
                        : text_kv_addresses->content_epoch(lane_state.kv->text, mapped_pages - 1U);
                const StateImageSelectors lane_selectors = state_selectors(lane_state);
                std::fprintf(stderr,
                             "[mat-debug] PREFILL-LANE lane=%u frontier=%u text_row=%d pages=%u "
                             "last_page_epoch=%llu src_slot=%d dst_slot=%d fork_pending=%d "
                             "rope_delta=%d\n",
                             other, lane_state.execution_frontier,
                             text_kv_addresses->bound_row(lane_state.kv->text), mapped_pages,
                             static_cast<unsigned long long>(last_page_epoch),
                             lane_selectors.source, lane_selectors.destination,
                             lane_state.state.fork_pending ? 1 : 0, lane_state.rope_delta);
                } catch (const std::exception& skip) {
                    // Never silent: a lane missing from the census must say why, otherwise a
                    // single-line census looks like "no concurrency" when it is really "skipped".
                    std::fprintf(stderr, "[mat-debug] PREFILL-CENSUS-SKIP lane=%u reason=%s\n",
                                 other, skip.what());
                    std::fflush(stderr);
                    continue;
                }
            }
            std::fflush(stderr);
        }
        install_sampling(sequence, request, request_plan.sampling);
        sequence.rope_delta = staged.prompt.rope_delta;
        set_device_i32(io.rope_delta, sequence.rope_delta);

        request.timings              = {};
        request.pending              = {};
        request.publish_continuation = request_plan.summary.publish_continuation;
        sequence.mtp_draft_count     = 0;
        sequence.tail_hidden_valid   = base == prompt_tokens && sequence.tail_hidden_valid;
        sequence.ledger.swap(materialization_ledger_);
        sequence.prefix_identity.swap(materialization_identity_);
        sequence.prefix_digests.swap(materialization_prefix_digests_);
        sequence.rebuild_work       = request_plan.root_rebuild_work;
        sequence.rebuild_tail_begin = request_plan.root_rebuild_tail_begin;

        if (is_masked_draft_backend(speculative_backend)) {
            if (!dflash || !io.dflash_decode || (backend_kv_cache() && !sequence.kv->backend)) {
                throw std::logic_error("DFlash prefill state is incomplete");
            }
            *dflash_host_ingress                       = {};
            dflash_host_ingress->active_lanes[0]       = static_cast<std::int32_t>(sequence.lane);
            const StateImageSelectors selectors        = state_selectors(sequence);
            dflash_host_ingress->state_source_slots[0] = selectors.source;
            dflash_host_ingress->state_destination_slots[0] = selectors.destination;
            dflash_host_ingress->dflash_kv_table_rows[0] =
                sequence.kv->backend ? backend_kv_addresses->bound_row(*sequence.kv->backend) : 0;
            CUDA_CHECK(cudaMemcpyAsync(io.dflash_decode->ingress.data, dflash_host_ingress,
                                       sizeof(qwen3_5::DFlashDecodeIngress), cudaMemcpyHostToDevice,
                                       device.stream));
        }

        staged.elapsed_seconds += std::chrono::duration<double>(Clock::now() - started).count();
        request.lifecycle = Lifecycle::Prefilling;
    } catch (...) {
        try {
            device.synchronize();
        } catch (...) {}
        clear_lane_best_effort(sequence, request);
        throw;
    }
}

runtime::PrefillStepResult
ProgramImpl::advance_prefill_raw(std::uint32_t lane, runtime::ExecutionTiming* failed_timing) {
    if (lane >= max_concurrency) { throw std::out_of_range("request lane is out of range"); }
    // Cross-step GPU dependency control (NINFER_SYNC_STEPS=1). Two concurrent canary runs with
    // byte-identical prompts produce *different* recurrent states for the same session at the
    // same step, while a non-overlapping session is bit-identical across the same runs -- i.e.
    // the nondeterminism needs overlapping work. If draining the device before and after every
    // prefill step makes the two concurrent runs identical (and removes the bleed), then a
    // prefill's kernels are still running while a later step's kernels share scratch with them.
    // N1: NOT fixed, and no drain is kept here. Five attempts failed to make the engine's steps
    // reproducible -- ordering the compute stream behind the transfer stream, a ready()-guarded
    // host sync, draining the transfer stream alone, draining before the step only, and draining
    // both sides of it. The both-sided device drain had appeared to give bit-identical runs twice,
    // but under the three-run standard the pairs differ (1, 171 and 170 of 234 step digests), i.e.
    // the pairs agreed by luck -- the same error the "located" claim was retracted for.
    //
    // The determinism oracle itself is now suspected: `STATE-SLOT` hashed 256 bytes from the START
    // of each layer's conv slot, and the conv window is written at column `pos % 3`, so offset 0 is
    // stale-dependent by construction and any two runs can differ without the engine being
    // nondeterministic. Fix the oracle (digest a fully-written region) before spending another run
    // on this.
    // Uninitialized-scratch control (NINFER_POISON_WORKSPACE=1). The engine produces a foreign
    // reply for a lane whose prompt, KV pages, device block table and state slots are all its own,
    // so the pollution is in memory no per-lane audit covers: the shared forward-pass workspace.
    // Filling it with a constant before each prefill step replaces whatever the previous occupant
    // -- possibly another lane -- left behind: if a kernel reads scratch it never wrote, the reply
    // changes (to fixed garbage or to the correct one); if nothing reads it, the reply is
    // unchanged and the arena is not the carrier.
    if (diagnostic_control_enabled("NINFER_POISON_WORKSPACE")) {
        CUDA_CHECK(cudaMemsetAsync(work.base(), 0xAB, work.capacity(), device.stream));
    }
    runtime::PrefillStepResult result =
        advance_prefill(active_sequence(lane), requests[lane], failed_timing);
    return result;
}

runtime::ExecutionTiming ProgramImpl::resolve_prefill_raw(std::uint32_t lane, bool terminal,
                                                          runtime::ExecutionTiming* failed_timing) {
    if (lane >= max_concurrency) { throw std::out_of_range("request lane is out of range"); }
    if (requests[lane].pending.kind != PendingKind::Begin) {
        throw std::logic_error("prefill resolution requires a pending prefill token");
    }
    return resolve_non_speculative_pending(active_sequence(lane), requests[lane], 1, terminal,
                                           std::nullopt, failed_timing);
}

runtime::ExecutionTiming ProgramImpl::resolve_pending_raw(
    std::span<const std::uint32_t> lanes, std::span<const std::uint32_t> accepted_tokens,
    std::span<const std::uint8_t> terminal, std::span<const std::uint8_t> cancelled,
    std::span<const std::optional<std::uint32_t>> prefix_execution_splits,
    runtime::ExecutionTiming* failed_timing) {
    runtime::ExecutionTimingRecorder timing(runtime::ExecutionTimingPhase::Post, failed_timing);
    if (lanes.empty() || lanes.size() > max_concurrency || accepted_tokens.size() != lanes.size() ||
        terminal.size() != lanes.size() || cancelled.size() != lanes.size() ||
        prefix_execution_splits.size() != lanes.size()) {
        throw std::invalid_argument("pending batch resolution has inconsistent membership");
    }

    if (lanes.size() == 1 && lanes.front() < max_concurrency &&
        requests[lanes.front()].pending.kind == PendingKind::Begin) {
        const std::uint32_t lane = lanes.front();
        if (requests[lane].lifecycle != Lifecycle::Pending) {
            throw std::logic_error("prefill pending token no longer matches Program state");
        }
        if (cancelled.front()) {
            if (accepted_tokens.front() != 0 || !terminal.front()) {
                throw std::logic_error("cancelled prefill pending decision is invalid");
            }
            if (!clear_lane_strict(active_sequence(lane), requests[lane])) {
                throw std::logic_error("cancelled prefill lane is not strictly releasable");
            }
        } else {
            timing.pause();
            timing.include(resolve_non_speculative_pending(
                active_sequence(lane), requests[lane], accepted_tokens.front(),
                terminal.front() != 0, prefix_execution_splits.front(), failed_timing));
            timing.resume_post();
        }
        return timing.finish();
    }

    if (speculative_backend == SpeculativeBackend::None) {
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            const std::uint32_t lane = lanes[row];
            if (lane >= max_concurrency || requests[lane].lifecycle != Lifecycle::Pending ||
                requests[lane].pending.kind != PendingKind::Ordinary) {
                throw std::logic_error("ordinary pending batch no longer matches Program state");
            }
            if (cancelled[row]) {
                if (!clear_lane_strict(active_sequence(lane), requests[lane])) {
                    throw std::logic_error("cancelled decode lane is not strictly releasable");
                }
            } else {
                timing.pause();
                timing.include(resolve_non_speculative_pending(
                    active_sequence(lane), requests[lane], accepted_tokens[row], terminal[row] != 0,
                    prefix_execution_splits[row], failed_timing));
                timing.resume_post();
            }
        }
        return timing.finish();
    }

    if (!replay_fold) {
        throw std::logic_error("speculative pending batch has no ReplaySSM records");
    }

    std::array<ops::GdnReplayFoldRow, kMaximumConcurrency> fold_rows{};
    std::array<std::int32_t, kMaximumConcurrency> hidden_selectors{};
    bool needs_hidden_correction = false;
    for (std::size_t row = 0; row < lanes.size(); ++row) {
        const std::uint32_t lane = lanes[row];
        if (lane >= max_concurrency || requests[lane].lifecycle != Lifecycle::Pending ||
            requests[lane].pending.kind != PendingKind::Speculative) {
            throw std::logic_error("speculative pending batch no longer matches Program state");
        }
        const PendingCandidate& pending = requests[lane].pending;
        const SequenceState& sequence   = active_sequence(lane);
        if (sequence.execution_frontier != pending.base_E ||
            sequence.ledger_frontier != pending.base_S ||
            sequence.ledger.size() != pending.base_S ||
            sequence.prefix_identity.size() != pending.base_S ||
            sequence.prefix_digests.size() != pending.base_S ||
            sequence.text_kv_valid != pending.base_E ||
            (speculative_backend == SpeculativeBackend::Mtp &&
             sequence.mtp_kv_valid != pending.base_E) ||
            (is_masked_draft_backend(speculative_backend) &&
             sequence.dflash_context_frontier != pending.base_E)) {
            throw std::logic_error("speculative pending row is not at its recorded base");
        }
        const std::uint32_t committed = cancelled[row] ? 0U : accepted_tokens[row];
        if ((cancelled[row] && accepted_tokens[row] != 0) ||
            (!cancelled[row] && (committed == 0 || committed > pending.produced ||
                                 (!terminal[row] && committed != pending.produced)))) {
            throw std::logic_error("speculative pending row has an invalid committed prefix");
        }
        const StateImageSelectors selectors = state_selectors(sequence);
        fold_rows[row] =
            ops::GdnReplayFoldRow{.source_state_slot      = selectors.source,
                                  .destination_state_slot = selectors.destination,
                                  .commit_columns         = static_cast<std::int32_t>(committed)};
        const bool partial_terminal =
            !cancelled[row] && terminal[row] && committed < pending.produced;
        hidden_selectors[row] =
            static_cast<std::int32_t>(partial_terminal ? committed - 1U : pending.produced - 1U);
        needs_hidden_correction = needs_hidden_correction || partial_terminal;
    }

    const auto tail_started = Clock::now();
    try {
        timing.resume_submit();
        replay_fold->execute(std::span<const ops::GdnReplayFoldRow>(fold_rows.data(), lanes.size()),
                             device.stream);

        // Sparse acceptance reads counts. Publish only the prefix licensed by the Frontend.
        if (speculative_backend == SpeculativeBackend::DFlash2) {
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                if (cancelled[row] || !requests[lanes[row]].sampling_host.token_counts) {
                    continue;
                }
                const auto count = static_cast<std::int32_t>(accepted_tokens[row]);
                Tensor ids =
                    io.dflash_decode->licensed_tokens.slice(1, static_cast<std::int32_t>(row), 1)
                        .slice(0, 0, count)
                        .view({count});
                Tensor counts =
                    token_counts.slice(1, static_cast<std::int32_t>(lanes[row]), 1)
                        .view({dimension(parameters.model.resources().public_token_count)});
                ops::increment_token_counts(ids, counts, device.stream);
            }
        }

        if (needs_hidden_correction) {
            const auto batch = static_cast<std::int32_t>(lanes.size());
            Tensor selector_tensor;
            Tensor hidden;
            Tensor selected;
            Tensor destinations;
            if (speculative_backend == SpeculativeBackend::Mtp && io.mtp_decode) {
                qwen3_5::MtpDecodeState& frame = *io.mtp_decode;
                selector_tensor                = frame.current_extents.slice(0, 0, batch);
                hidden                         = frame.target_hidden.slice(2, 0, batch);
                selected     = frame.target_continuation_hidden.slice(1, 0, batch);
                destinations = frame.state_destination_slots.slice(0, 0, batch);
            } else if (is_masked_draft_backend(speculative_backend) && io.dflash_decode) {
                qwen3_5::DFlashDecodeState& frame = *io.dflash_decode;
                selector_tensor                   = frame.proposal_extents.slice(0, 0, batch);
                hidden                            = frame.target_hidden.slice(2, 0, batch);
                selected     = frame.target_continuation_hidden.slice(1, 0, batch);
                destinations = frame.state_destination_slots.slice(0, 0, batch);
            } else {
                throw std::logic_error("partial speculative commit has no target frame");
            }
            // Async + settle: `hidden_selectors` is a block-local std::array.
            CUDA_CHECK(cudaMemcpyAsync(selector_tensor.data, hidden_selectors.data(),
                                       lanes.size() * sizeof(std::int32_t), cudaMemcpyHostToDevice,
                                       device.stream));
            CUDA_CHECK(cudaStreamSynchronize(device.stream));
            ops::speculative_select_accepted_hidden(hidden, selector_tensor, selected,
                                                    device.stream);
            ops::scatter(selected, destinations, state_images->continuation_hidden_store(),
                         device.stream);
        }

        if (is_masked_draft_backend(speculative_backend)) {
            std::array<std::uint32_t, kMaximumConcurrency> append_lanes{};
            std::array<std::uint32_t, kMaximumConcurrency> append_starts{};
            std::array<std::uint32_t, kMaximumConcurrency> append_counts{};
            std::size_t append_size = 0;
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                if (!cancelled[row] && terminal[row]) {
                    append_lanes[append_size]  = lanes[row];
                    append_starts[append_size] = requests[lanes[row]].pending.base_E;
                    append_counts[append_size] = accepted_tokens[row];
                    ++append_size;
                }
            }
            if (append_size != 0) {
                enqueue_dflash_context_append(
                    std::span<const std::uint32_t>(append_lanes.data(), append_size),
                    std::span<const std::uint32_t>(append_starts.data(), append_size),
                    std::span<const std::uint32_t>(append_counts.data(), append_size));
            }
        }

        timing.begin_wait();
        device.synchronize();
        timing.end_wait();
        work.reset();
    } catch (...) {
        try {
            device.synchronize();
        } catch (...) {}
        work.reset();
        clear_execution_failure_lanes(lanes);
        throw;
    }

    const double tail_seconds = std::chrono::duration<double>(Clock::now() - tail_started).count();
    const std::uint32_t width = draft_window + 1U;
    try {
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            SequenceState& sequence = active_sequence(lanes[row]);
            RequestControl& request = requests[lanes[row]];
            if (cancelled[row]) {
                if (!clear_lane_strict(sequence, request)) {
                    throw std::logic_error("cancelled speculative lane is not strictly releasable");
                }
                continue;
            }

            const PendingCandidate pending = request.pending;
            const std::uint32_t committed  = accepted_tokens[row];
            settle_state_fork(sequence);
            const TokenId* token_base =
                speculative_backend == SpeculativeBackend::Mtp
                    ? mtp_host_egress->licensed_tokens.data() + row * width
                    : dflash_host_egress->licensed_tokens.data() + row * width;
            sequence.ledger.insert(sequence.ledger.end(), token_base, token_base + committed);
            commit_generated_prefix_identity(sequence, pending.base_S,
                                             std::span<const TokenId>(token_base, committed),
                                             prefix_execution_splits[row]);
            advance_rebuild_work(sequence, pending.base_E + committed, prefill_chunk);
            sequence.execution_frontier = pending.base_E + committed;
            sequence.ledger_frontier    = pending.base_S + committed;
            sequence.text_kv_valid      = sequence.execution_frontier;
            sequence.tail_hidden_valid  = true;

            if (speculative_backend == SpeculativeBackend::Mtp) {
                sequence.mtp_kv_valid = sequence.execution_frontier;
                if (terminal[row]) {
                    sequence.mtp_draft_count = 0;
                } else {
                    const std::int32_t next  = mtp_host_egress->next_extents[row];
                    sequence.mtp_draft_count = static_cast<std::uint32_t>(next);
                    for (std::uint32_t step = 0; step < sequence.mtp_draft_count; ++step) {
                        sequence.mtp_drafts[step] =
                            mtp_host_egress->next_drafts[step * max_concurrency + row];
                    }
                }
            } else {
                sequence.dflash_context_frontier =
                    terminal[row] ? sequence.execution_frontier : pending.base_E;
            }

            commit_sequence_kv(sequence, sequence.text_kv_valid, backend_kv_valid(sequence));
            trim_sequence_kv(sequence, sequence.text_kv_valid, backend_kv_valid(sequence));
            if (terminal[row]) {
                request.lifecycle = Lifecycle::Finishable;
            } else {
                request.lifecycle = Lifecycle::Active;
            }
            request.pending = {};
            request.timings.decode_seconds += tail_seconds;
        }
    } catch (...) {
        clear_execution_failure_lanes(lanes);
        throw;
    }
    return timing.finish();
}

runtime::PrefillStepResult ProgramImpl::advance_prefill(SequenceState& sequence,
                                                        RequestControl& request,
                                                        runtime::ExecutionTiming* failed_timing) {
    runtime::ExecutionTimingRecorder timing(runtime::ExecutionTimingPhase::Submit, failed_timing);
    if (request.lifecycle != Lifecycle::Prefilling || !request.prefill) {
        throw std::logic_error("staged prefill step requires an active concurrent request");
    }

    RequestControl::Prefill& staged = *request.prefill;
    if (staged.pending_capture_offer != 0) {
        throw std::logic_error("prefill cannot advance while a capture offer is pending");
    }
    // Re-bind THIS lane's KV row at every prefill step.
    //
    // The prefill forward pass does not use the lane-specific KV view it is handed: `TextContext`
    // stores `state.text_kv` as `kv_` and never reads it, and `attn_mix` takes the row from the single
    // shared device scalar `io_.text_kv_table_row` instead. Nothing rewrote that scalar during a
    // prefill -- only materialization and capture publication call `bind_sequence_kv` -- so as soon as
    // the next lane was admitted while this lane was still prefilling, this lane's remaining chunks
    // wrote and attended through THAT lane's row. One session's document landed in another's pages,
    // which is D2: the victim continues with the previously admitted session's content, the last
    // admitted lane looks clean (its own prefill overwrites the borrowed row), and column-zero-ish
    // lanes lose the writes that were redirected.
    //
    // Every audit missed it because they all checked KV *bindings* -- page identity, block tables,
    // table contents -- and the decode ingress, never the row *selector* the prefill kernel reads.
    // Idempotent for an active address (it skips `activate`), so calling it here costs two 4-byte
    // device writes per step.
    // W1-A: what the host record says the scalars named when this step began, judged against this
    // lane's own rows. One comparison and two increments, always on. A foreign reading here is the
    // pre-2026-09-25 condition -- the state the D2 fix exists to correct -- but the trigger is
    // narrower than "another lane is running": `bind_sequence_kv` is called only from this step, from
    // materialization and from shared-prefix publication, so a foreign reading means another lane
    // was admitted or published a shared prefix between two of this lane's steps. Measured over the
    // prod4 run of 2026-09-25: 0 of 512. So this is a recurrence detector with a low trigger rate,
    // and 0 of N means the trigger did not occur, NOT that a guard passed. An earlier version of
    // this comment claimed the count was expected to be high under concurrency; the run says
    // otherwise, and the claim was wrong rather than the measurement.
    {
        const std::pair<std::int32_t, std::int32_t> step_rows = bound_kv_rows(sequence);
        kv_row_binding_.observe(sequence.lane, step_rows.first, step_rows.second);
    }
    bind_sequence_kv(sequence);
    // The DFlash sink's ingress is shared device state with the same exposure as the row scalars, and
    // it must be re-published here for the same reason: this is the path a long prompt takes, and the
    // fix that claimed to cover it only covered materialization (see the note in `start_sequence`).
    bind_dflash_prefill_sink(sequence);
    // W1-A: what the *device* says the two scalars name, now that this lane has re-bound them. This
    // is the only form that can catch a second writer -- the host record agrees with itself by
    // construction, and the D2 fix rests on `bind_sequence_kv` being the sole writer of both.
    if (std::getenv("NINFER_KV_BINDING_CHECK") != nullptr) {
        std::int32_t device_text_row    = -1;
        std::int32_t device_backend_row = -1;
        // `set_device_i32` settles the stream, so the scalars hold what the host last wrote and no
        // extra ordering is needed beyond this copy's own.
        CUDA_CHECK(cudaMemcpyAsync(&device_text_row, io.text_kv_table_row.data,
                                   static_cast<std::size_t>(sizeof(device_text_row)),
                                   cudaMemcpyDeviceToHost, device.stream));
        CUDA_CHECK(cudaMemcpyAsync(&device_backend_row, io.backend_kv_table_row.data,
                                   static_cast<std::size_t>(sizeof(device_backend_row)),
                                   cudaMemcpyDeviceToHost, device.stream));
        CUDA_CHECK(cudaStreamSynchronize(device.stream));
        const bool device_agrees = kv_row_binding_.verify(device_text_row, device_backend_row);
        if (!device_agrees && kv_row_binding_.diverged_readings() <= 8) {
            std::fprintf(stderr,
                         "[kv-binding] DEVICE-DIVERGED lane=%u device=(%d,%d) recorded=(%d,%d) "
                         "diverged=%llu/%llu\n",
                         sequence.lane, device_text_row, device_backend_row,
                         kv_row_binding_.text_row(), kv_row_binding_.backend_row(),
                         static_cast<unsigned long long>(kv_row_binding_.diverged_readings()),
                         static_cast<unsigned long long>(kv_row_binding_.verified_readings()));
            std::fflush(stderr);
        }
        const std::uint64_t verified = kv_row_binding_.verified_readings();
        // `verified % 512 == 0` alone was a rate limit that swallowed the denominator: the counter
        // starts at 1 after the first check, so only a run that reached *exactly* a multiple of 512
        // printed anything -- the prod4 run of 14:22 printed once at 512, the one at 14:31 printed
        // nothing, and the silence read as "nothing to report". Print the first reading
        // unconditionally, then every 512th, and every divergence.
        if (verified == 1ULL || verified % 512ULL == 0ULL || !device_agrees) {
            std::fprintf(stderr,
                         "[kv-binding] lane=%u checks=%llu diverged=%llu foreign_rebinds=%llu/%llu "
                         "device=(%d,%d)\n",
                         sequence.lane, static_cast<unsigned long long>(verified),
                         static_cast<unsigned long long>(kv_row_binding_.diverged_readings()),
                         static_cast<unsigned long long>(kv_row_binding_.foreign_observations()),
                         static_cast<unsigned long long>(kv_row_binding_.total_observations()),
                         device_text_row, device_backend_row);
            std::fflush(stderr);
        }
    }
    // Admission-side prompt tail (NINFER_LEDGER_PROBE=1): the same 64-entry hash the decode-side
    // LEDGER-FP prints, over the same region (the tail of the admitted prompt), so the two can be
    // compared per lane without any cross-lane alignment. The canary sits at the prompt's END, so
    // this is the region a contaminated ledger would have to differ in.
    if (std::getenv("NINFER_LEDGER_PROBE") != nullptr) {
        try {
            const auto& ids = staged.prompt.token_ids;
            std::uint64_t tail_hash    = 1469598103934665603ULL;
            std::uint32_t tail_checked = 0;
            const std::uint32_t tail =
                std::min<std::uint32_t>(64U, staged.prompt_tokens);
            for (std::uint32_t i = 0; i < tail; ++i) {
                const std::size_t index = static_cast<std::size_t>(staged.prompt_tokens - 1U - i);
                if (index >= ids.size()) { break; }
                tail_hash ^= static_cast<std::uint64_t>(ids[index]);
                tail_hash *= 1099511628211ULL;
                ++tail_checked;
            }
            std::fprintf(stderr,
                         "[mat-debug] PROMPT-TAIL64 lane=%u admitted=%u ids=%zu tail64=%llx "
                         "tail_checked=%u\n",
                         sequence.lane, staged.prompt_tokens, ids.size(),
                         static_cast<unsigned long long>(tail_hash), tail_checked);
            std::fflush(stderr);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[mat-debug] PROMPT-TAIL64-SKIP lane=%u reason=%s\n", sequence.lane,
                         error.what());
            std::fflush(stderr);
        }
    }
    const runtime::BeginSummary summary{.prompt_tokens        = staged.prompt_tokens,
                                        .reused_prompt_tokens = staged.base,
                                        .prefix_reuse_path    = staged.reuse};
    std::uint32_t processed_prompt_tokens = 0;
    const auto started                    = Clock::now();
    try {
        if (staged.next_capture < staged.capture_groups.size() &&
            staged.capture_groups[staged.next_capture].frontier == staged.cursor) {
            if (staged.cursor != staged.base ||
                !staged.capture_groups[staged.next_capture].shared ||
                staged.capture_groups[staged.next_capture].rewrite ||
                staged.capture_groups[staged.next_capture].long_anchor) {
                throw std::logic_error("zero-prefill capture is not a shared base promotion");
            }
            if (++next_capture_offer_id_ == 0) { ++next_capture_offer_id_; }
            staged.pending_capture_offer = next_capture_offer_id_;
            return runtime::PrefillStepResult{
                .summary = summary,
                .timing  = timing.finish(),
            };
        }
        StateImageSelectors selectors = state_selectors(sequence);
        Tensor rewrite_capture_hidden;
        Tensor* rewrite_capture_hidden_ptr = nullptr;
        if (staged.next_capture < staged.capture_groups.size()) {
            rewrite_capture_hidden = state_images->continuation_hidden_slot(selectors.destination);
            rewrite_capture_hidden_ptr = &rewrite_capture_hidden;
        }
        execution::PrefillContext schedule_state{
            {device, parameters, work, state_images->linear(),
             replay_records ? &*replay_records : nullptr, io, prefill_hidden, prefill_chunk,
             proposal_head, rope_scaling_factor, rope_scaling_original_context},
            text_kv_view(sequence),
            mtp_kv_view(sequence),
            decoder->text_kv,
            decoder->mtp_cache(),
            dflash ? &*dflash : nullptr,
            staged.cursor,
            static_cast<const ops::SamplingConfig*>(
                sampling_config.slice(1, static_cast<std::int32_t>(sequence.lane), 1).data),
            rewrite_capture_hidden_ptr,
            selectors.source,
            selectors.destination,
            staged.initial_mtp_extent,
            dflash_host_ingress};

        if (staged.mtp_bridge == MtpBridgeMode::BeforeSuffix) {
            if (staged.cursor != staged.base || staged.base == 0 ||
                staged.cursor >= staged.prompt_tokens) {
                throw std::logic_error("staged MTP bridge is outside the reusable suffix");
            }
            mark_workspace_usage(workspace_plan.mtp_prefill);
            const Tensor& previous_hidden = sequence.tail_hidden;
            const execution::MtpBridgeInput bridge{
                .previous_hidden = &previous_hidden,
                .position        = checked_i32(staged.base - 1, "MTP bridge position"),
                .rope_position   = [&] {
                    auto rp = prompt_rope_position(staged.prompt, staged.base - 1);
                    for (std::int32_t& axis : rp) {
                        axis = yarn_scale_position(axis, rope_scaling_original_context,
                                                   rope_scaling_factor);
                    }
                    return rp;
                }(),
            };
            if (staged.vision) {
                execution::mtp_bridge_multimodal(schedule_state, staged.prompt, *staged.vision,
                                                 bridge);
            } else {
                Tensor bridge_token = io.mtp->target_input_ids.slice(0, 0, 1);
                const TokenId token = staged.prompt.token_ids[staged.base];
                // Async + settle: stack-local source, non-blocking stream.
                CUDA_CHECK(cudaMemcpyAsync(bridge_token.data, &token, sizeof(token),
                                           cudaMemcpyHostToDevice, device.stream));
                CUDA_CHECK(cudaStreamSynchronize(device.stream));
                execution::mtp_bridge_and_propose(schedule_state, bridge_token, previous_hidden,
                                                  bridge.position, bridge.rope_position, false);
            }
            sequence.mtp_kv_valid = staged.base;
            commit_sequence_kv(sequence, sequence.text_kv_valid, sequence.mtp_kv_valid);
            staged.mtp_bridge = MtpBridgeMode::None;
        }

        if (staged.cursor < staged.prompt_tokens) {
            const std::uint32_t nominal =
                std::min(prefill_chunk, staged.prompt_tokens - staged.cursor);
            mark_workspace_usage(staged.prepare_mtp ? workspace_plan.mtp_prefill
                                                    : workspace_plan.text_prefill);
            if (is_masked_draft_backend(speculative_backend)) {
                mark_workspace_usage(workspace_plan.dflash_context);
            }
            std::uint32_t remaining          = nominal;
            std::uint32_t final_chunk_tokens = 0;
            bool finalized                   = false;
            while (remaining != 0) {
                schedule_state.text_kv_base           = staged.cursor;
                selectors                             = state_selectors(sequence);
                schedule_state.state_source_slot      = selectors.source;
                schedule_state.state_destination_slot = selectors.destination;
                if (staged.next_capture < staged.capture_groups.size()) {
                    rewrite_capture_hidden =
                        state_images->continuation_hidden_slot(selectors.destination);
                    schedule_state.rewrite_checkpoint_hidden = &rewrite_capture_hidden;
                } else {
                    schedule_state.rewrite_checkpoint_hidden = nullptr;
                }

                const bool final_candidate = staged.cursor + remaining == staged.prompt_tokens;
                const std::optional<std::uint32_t> capture_frontier =
                    staged.next_capture < staged.capture_groups.size()
                        ? std::optional<std::uint32_t>(
                              staged.capture_groups[staged.next_capture].frontier)
                        : std::nullopt;
                std::optional<std::uint32_t> split_frontier = capture_frontier;
                const auto rewrite_split                    = std::upper_bound(
                    staged.prompt.identity.rewrite_execution_frontiers.begin(),
                    staged.prompt.identity.rewrite_execution_frontiers.end(), staged.cursor);
                if (rewrite_split != staged.prompt.identity.rewrite_execution_frontiers.end() &&
                    (!split_frontier || *rewrite_split < *split_frontier)) {
                    split_frontier = *rewrite_split;
                }
                execution::PrefillChunkResult result;
                timing.pause();
                if (staged.vision) {
                    if (!workspace_plan.vision) {
                        throw std::logic_error("active Vision prefill lost its workspace plan");
                    }
                    mark_workspace_usage(workspace_plan.vision->capacity_bytes);
                    result = execution::prefill_multimodal_chunk(schedule_state, staged.prompt,
                                                                 *staged.vision, remaining,
                                                                 split_frontier, final_candidate);
                } else {
                    result = execution::prefill_text_chunk(
                        schedule_state, std::span<const TokenId>(staged.prompt.token_ids),
                        remaining, split_frontier, final_candidate);
                }
                timing.include(result.timing);
                timing.resume_post();
                if (result.processed_tokens == 0 || result.processed_tokens > remaining) {
                    throw std::logic_error("ordinary prefill chunk made invalid progress");
                }
                if (staged.vision) { staged.vision->release_encoded_media_payloads(); }
                staged.cursor += result.processed_tokens;
                processed_prompt_tokens += result.processed_tokens;
                remaining -= result.processed_tokens;
                final_chunk_tokens     = result.processed_tokens;
                sequence.text_kv_valid = staged.cursor;
                if (staged.prepare_mtp) { sequence.mtp_kv_valid = staged.cursor; }
                if (is_masked_draft_backend(speculative_backend)) {
                    sequence.dflash_context_frontier = staged.cursor;
                }
                commit_sequence_kv(sequence, sequence.text_kv_valid, backend_kv_valid(sequence));

                // Prompt transitions are canonical immediately. If this was the first write after
                // an immutable source, close the Fork before potentially freezing a new rewrite.
                settle_state_fork(sequence);
                const bool reached_capture = capture_frontier && staged.cursor == *capture_frontier;
                if (reached_capture) {
                    if (result.finalized) {
                        // The prompt-frontier state becomes publishable only after the generated
                        // Begin token is committed. commit() emits the offer for this group.
                    } else {
                        staged.elapsed_seconds +=
                            std::chrono::duration<double>(Clock::now() - started).count();
                        if (++next_capture_offer_id_ == 0) { ++next_capture_offer_id_; }
                        staged.pending_capture_offer = next_capture_offer_id_;
                        return runtime::PrefillStepResult{
                            .summary                 = summary,
                            .processed_prompt_tokens = processed_prompt_tokens,
                            .timing                  = timing.finish(),
                        };
                    }
                }

                finalized = result.finalized;
                if (finalized || remaining == 0) { break; }
            }

            if (!finalized) {
                if (staged.cursor == staged.prompt_tokens) {
                    throw std::logic_error("staged prefill reached the prompt without sampling");
                }
                staged.elapsed_seconds +=
                    std::chrono::duration<double>(Clock::now() - started).count();
                return runtime::PrefillStepResult{
                    .summary                 = summary,
                    .processed_prompt_tokens = processed_prompt_tokens,
                    .timing                  = timing.finish(),
                };
            }
            if (staged.cursor != staged.prompt_tokens) {
                throw std::logic_error("staged prefill sampled before the prompt frontier");
            }
            timing.resume_submit();
            copy_tail(sequence, prefill_hidden.slice(
                                    1, static_cast<std::int32_t>(final_chunk_tokens) - 1, 1));
        } else {
            mark_workspace_usage(workspace_plan.ordinary_round);
            if (!sequence.tail_hidden_valid) {
                throw std::logic_error("zero-suffix reuse has no target tail hidden");
            }
            execution::sample_from_hidden(schedule_state, sequence.tail_hidden,
                                          checked_i32(staged.prompt_tokens, "sample position"),
                                          ops::kSamplePurposePrefill);
            set_device_i32(io.rope_pos,
                           yarn_scale_position(
                               checked_i32(staged.prompt_tokens, "rope position") +
                                   sequence.rope_delta,
                               rope_scaling_original_context, rope_scaling_factor));
            if (staged.prepare_mtp) {
                if (staged.mtp_bridge != MtpBridgeMode::AfterExactHit) {
                    throw std::logic_error("zero-suffix MTP reuse has no exact-hit bridge");
                }
                mark_workspace_usage(workspace_plan.mtp_prefill);
                const auto bridge_rope = [&] {
                    auto rp = prompt_rope_position(staged.prompt, staged.prompt_tokens - 1);
                    for (std::int32_t& axis : rp) {
                        axis = yarn_scale_position(axis, rope_scaling_original_context,
                                                   rope_scaling_factor);
                    }
                    return rp;
                }();
                execution::mtp_bridge_and_propose(
                    schedule_state, io.token, sequence.tail_hidden,
                    checked_i32(staged.prompt_tokens - 1, "MTP full-prefix bridge position"),
                    bridge_rope, staged.initial_mtp_extent != 0);
                sequence.mtp_kv_valid = staged.prompt_tokens;
                commit_sequence_kv(sequence, sequence.text_kv_valid, sequence.mtp_kv_valid);
                staged.mtp_bridge = MtpBridgeMode::None;
            }
        }

        copy_round_token();
        std::array<TokenId, qwen3_5::kMtpDecodeMaximumDrafts> initial_drafts{};
        if (staged.prepare_mtp && staged.initial_mtp_extent != 0) {
            CUDA_CHECK(cudaMemcpyAsync(initial_drafts.data(), io.mtp->draft_tokens.data,
                                       staged.initial_mtp_extent * sizeof(TokenId),
                                       cudaMemcpyDeviceToHost, device.stream));
        }
        timing.begin_wait();
        device.synchronize();
        timing.end_wait();
        staged.elapsed_seconds += std::chrono::duration<double>(Clock::now() - started).count();
        const double vision_seconds       = staged.vision ? staged.vision->elapsed_seconds() : 0.0;
        const std::uint32_t prompt_tokens = staged.prompt_tokens;

        validate_licensed_tokens(std::span<const TokenId>(host_tokens, 1));
        if (sequence.ledger.size() != prompt_tokens) {
            throw std::logic_error("candidate token ledger does not match prompt length");
        }
        sequence.ledger.push_back(host_tokens[0]);
        sequence.prefix_identity.append_generated(1, sequence.rope_delta);
        sequence.prefix_digests.append_generated(std::span<const TokenId>(host_tokens, 1),
                                                 sequence.rope_delta);
        sequence.text_kv_valid = prompt_tokens;
        if (staged.prepare_mtp) {
            if (sequence.mtp_kv_valid != prompt_tokens) {
                throw std::logic_error("staged MTP prefill did not reach the prompt frontier");
            }
            sequence.mtp_draft_count = staged.initial_mtp_extent;
            std::copy_n(initial_drafts.begin(), staged.initial_mtp_extent,
                        sequence.mtp_drafts.begin());
        } else if (is_masked_draft_backend(speculative_backend) &&
                   sequence.dflash_context_frontier != prompt_tokens) {
            throw std::logic_error("staged DFlash prefill did not reach the prompt frontier");
        }
        sequence.tail_hidden_valid      = true;
        request.timings.vision_seconds  = vision_seconds;
        request.timings.prefill_seconds = std::max(0.0, staged.elapsed_seconds - vision_seconds);
        staged.prompt.release_all_media_payloads();
        if (staged.vision) { staged.vision->retire_handoff(); }

        const bool prompt_frontier_capture =
            staged.next_capture < staged.capture_groups.size() &&
            staged.capture_groups[staged.next_capture].frontier == prompt_tokens;
        if (!prompt_frontier_capture) { request.prefill.reset(); }
        request.pending   = PendingCandidate{.kind          = PendingKind::Begin,
                                             .base_E        = 0,
                                             .base_S        = 0,
                                             .prompt_tokens = prompt_tokens,
                                             .produced      = 1};
        request.lifecycle = Lifecycle::Pending;
        return runtime::PrefillStepResult{
            .summary = summary,
            .round   = runtime::GeneratedRound{.tokens = std::span<const TokenId>(host_tokens, 1)},
            .processed_prompt_tokens = processed_prompt_tokens,
            .complete                = true,
            .timing                  = timing.finish(),
        };
    } catch (...) {
        timing.begin_wait();
        try {
            device.synchronize();
        } catch (...) {}
        timing.end_wait();
        const std::uint32_t lane = sequence.lane;
        clear_execution_failure_lanes(std::span<const std::uint32_t>(&lane, 1));
        throw;
    }
}


} // namespace ninfer::models::qwen3_5::detail
