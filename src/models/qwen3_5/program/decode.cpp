#include "models/qwen3_5/program/program_impl.h"
#include "models/qwen3_5/program/context_work.h"
#include "models/qwen3_5/program/context.h"
#include "models/qwen3_5/program/graph_execution.h"
#include "core/nvtx.h"
#include "core/device.h"
#include "core/diagnostics.h"
#include "ninfer/ops/prepare_ragged_prefix.h"
#include "ninfer/ops/sampling.h"
#include "ninfer/ops/scatter.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ninfer::models::qwen3_5::execution {
namespace {

auto ordinary_batch_body(OrdinaryBatchContext& state, std::int32_t batch_size,
                         ops::CausalAttentionExecutionEnvelope envelope) {
    return [&state, batch_size, envelope] {
        if (batch_size <= 0 || batch_size > static_cast<std::int32_t>(kMaximumConcurrency)) {
            throw std::logic_error("ordinary decode batch state is incomplete");
        }

        qwen3_5::OrdinaryDecodeState& ordinary = state.frame;
        CUDA_CHECK(cudaMemcpyAsync(ordinary.ingress.data, &state.host_ingress,
                                   sizeof(qwen3_5::OrdinaryDecodeIngress), cudaMemcpyHostToDevice,
                                   state.execution.device.stream));
        if (state.ingress_shadow != nullptr) {
            CUDA_CHECK(cudaMemcpyAsync(state.ingress_shadow, ordinary.ingress.data,
                                       sizeof(qwen3_5::OrdinaryDecodeIngress),
                                       cudaMemcpyDeviceToDevice, state.execution.device.stream));
        }

        TextContext card(state.execution.device, state.execution.parameters, state.execution.work,
                         {}, state.execution.linear_attention, state.execution.io,
                         state.execution.prefill_hidden, state.execution.prefill_chunk, 0, {},
                         &state.text_cache);

        Tensor tokens             = ordinary.tokens.slice(0, 0, batch_size);
        Tensor cache_positions    = ordinary.cache_positions.slice(0, 0, batch_size);
        Tensor rope_positions     = ordinary.rope_positions.slice(0, 0, batch_size);
        Tensor kv_rows            = ordinary.text_kv_table_rows.slice(0, 0, batch_size);
        Tensor state_sources      = ordinary.state_source_slots.slice(0, 0, batch_size);
        Tensor state_destinations = ordinary.state_destination_slots.slice(0, 0, batch_size);
        Tensor hidden             = ordinary.hidden.slice(1, 0, batch_size);
        Tensor logits             = ordinary.logits.slice(1, 0, batch_size);
        Tensor sampled            = ordinary.sampled_tokens.slice(0, 0, batch_size);
        Tensor valid_columns      = ordinary.valid_columns.slice(0, 0, batch_size);

        card.ordinary_decode_batch(tokens, cache_positions, rope_positions, valid_columns, kv_rows,
                                   state_sources, state_destinations, envelope, hidden, logits);
        ops::scatter(hidden, state_destinations, state.continuation_hidden_store,
                     state.execution.device.stream);
        ops::sample(logits, sampled,
                    dimension(state.execution.parameters.model.resources().public_token_count),
                    ordinary.sampling, cache_positions, ops::kSamplePurposeDecode,
                    state.execution.work, state.execution.device.stream);
        CUDA_CHECK(cudaMemcpyAsync(&state.host_egress, ordinary.egress.data,
                                   sizeof(qwen3_5::OrdinaryDecodeEgress), cudaMemcpyDeviceToHost,
                                   state.execution.device.stream));
    };
}

} // namespace

void capture_ordinary_decode_batch(OrdinaryBatchContext& state, std::int32_t batch_size,
                                   ops::CausalAttentionExecutionEnvelope envelope,
                                   DecodeGraphDefinition& definition) {
    auto body = ordinary_batch_body(state, batch_size, envelope);
    capture_graph(state, definition, body);
}

void ordinary_decode_batch(OrdinaryBatchContext& state, std::int32_t batch_size,
                           ops::CausalAttentionExecutionEnvelope envelope,
                           DecodeGraphExecutable* executable) {
    auto body = ordinary_batch_body(state, batch_size, envelope);
    run_prepared(state, executable, body);
}

} // namespace ninfer::models::qwen3_5::execution

namespace ninfer::models::qwen3_5::detail {

namespace {

// Host-side mirror of ops::scale_positions_yarn (src/ops/kernel/position.cuh). Applied to the
// spec-decode target and ordinary-decode host RoPE positions so the target model sees YaRN-scaled
// positions while the draft continues on unscaled logical positions (which it must, per the draft's
// own position-encoding invariants).
[[nodiscard]] std::int32_t yarn_scale_position(std::int32_t position, std::uint32_t original_context,
                                               float factor) noexcept {
    if (factor == 1.0F ||
        position <= static_cast<std::int32_t>(original_context)) {
        return position;
    }
    const float scaled = static_cast<float>(original_context) +
                         (static_cast<float>(position - static_cast<std::int32_t>(original_context))) /
                             factor +
                         0.5F;
    return static_cast<std::int32_t>(scaled);
}

DecodeGraphProfile& select_graph_profile(DecodeGraphFamily& family, std::uint32_t batch_size,
                                         std::uint32_t frontier, const char* label);

DecodeGraphTopology& select_graph_topology(DecodeGraphFamily& family, std::uint32_t topology_class,
                                           const char* label);

DecodeGraphExecutable& install_graph_profile(DecodeGraphFamily& family, DecodeGraphProfile& profile,
                                             const char* label);

DecodeGraphProfile& select_graph_profile(DecodeGraphFamily& family, std::uint32_t batch_size,
                                         std::uint32_t frontier, const char* label) {
    const auto it = std::find_if(
        family.profiles.begin(), family.profiles.end(), [&](const DecodeGraphProfile& profile) {
            return profile.batch_size == batch_size && profile.min_execution_frontier <= frontier &&
                   frontier <= profile.max_execution_frontier;
        });
    if (it == family.profiles.end()) {
        throw std::logic_error(std::string(label) + " CUDA Graph coverage is incomplete");
    }
    return *it;
}

DecodeGraphTopology& select_graph_topology(DecodeGraphFamily& family, std::uint32_t topology_class,
                                           const char* label) {
    const auto it = std::find_if(family.topologies.begin(), family.topologies.end(),
                                 [topology_class](const DecodeGraphTopology& topology) {
                                     return topology.topology_class == topology_class;
                                 });
    if (it == family.topologies.end()) {
        throw std::logic_error(std::string(label) + " CUDA Graph topology is unavailable");
    }
    return *it;
}

DecodeGraphExecutable& install_graph_profile(DecodeGraphFamily& family, DecodeGraphProfile& profile,
                                             const char* label) {
    DecodeGraphTopology& topology   = select_graph_topology(family, profile.topology_class, label);
    const std::size_t profile_index = static_cast<std::size_t>(&profile - family.profiles.data());
    if (topology.installed_profile != profile_index) {
        topology.executable.update(profile.definition);
        topology.installed_profile = profile_index;
    }
    return topology.executable;
}

} // namespace

void ProgramImpl::install_sampling(SequenceState& sequence, RequestControl& request,
                                   const ops::SamplingConfig& config) {
    Tensor counts = token_counts.slice(1, static_cast<std::int32_t>(sequence.lane), 1)
                        .view({dimension(parameters.model.resources().public_token_count)});
    request.sampling_host     = config;
    request.speculative_stats = SpeculativeStats{
        .backend               = speculative_backend,
        .enabled               = speculative_backend != SpeculativeBackend::None,
        .draft_window          = draft_window,
        .accepted_per_position = std::vector<std::uint64_t>(draft_window, 0),
    };
    const bool penalties = request.sampling_host.presence_penalty != 0.0F ||
                           request.sampling_host.frequency_penalty != 0.0F;
    if (penalties) { CUDA_CHECK(cudaMemsetAsync(counts.data, 0, counts.bytes(), device.stream)); }
    request.sampling_host.token_counts =
        penalties ? static_cast<std::int32_t*>(counts.data) : nullptr;
    Tensor config_lane = sampling_config.slice(1, static_cast<std::int32_t>(sequence.lane), 1);
    CUDA_CHECK(cudaMemcpyAsync(config_lane.data, &request.sampling_host,
                               sizeof(request.sampling_host), cudaMemcpyHostToDevice,
                               device.stream));
}

void ProgramImpl::copy_tail(SequenceState& sequence, const Tensor& source) {
    if (source.dtype != DType::BF16 ||
        source.ne[0] != dimension(parameters.model.config().text.hidden_size) ||
        source.ne[1] != 1) {
        throw std::logic_error("target tail hidden has an invalid shape");
    }
    CUDA_CHECK(cudaMemcpyAsync(sequence.tail_hidden.data, source.data, sequence.tail_hidden.bytes(),
                               cudaMemcpyDeviceToDevice, device.stream));
    sequence.tail_hidden_valid = true;
}

void ProgramImpl::copy_round_token() {
    CUDA_CHECK(cudaMemcpyAsync(host_tokens, io.token.data, sizeof(TokenId), cudaMemcpyDeviceToHost,
                               device.stream));
}

void ProgramImpl::mark_workspace_usage(std::size_t phase_bytes) noexcept {
    workspace_logical_peak_bytes = std::max(workspace_logical_peak_bytes, phase_bytes);
}

void ProgramImpl::enqueue_dflash_context_append(std::span<const std::uint32_t> lanes,
                                                std::span<const std::uint32_t> starts,
                                                std::span<const std::uint32_t> counts) {
    if (!is_masked_draft_backend(speculative_backend) || !dflash || !io.dflash_decode ||
        lanes.empty() || lanes.size() > max_concurrency || starts.size() != lanes.size() ||
        counts.size() != lanes.size()) {
        throw std::logic_error("DFlash context append has invalid membership");
    }

    std::uint32_t minimum_count = draft_window + 1U;
    std::uint32_t maximum_count = 0;
    *dflash_host_ingress        = {};
    for (std::size_t row = 0; row < lanes.size(); ++row) {
        const std::uint32_t lane = lanes[row];
        if (lane >= max_concurrency || counts[row] == 0 || counts[row] > draft_window + 1U ||
            std::find(lanes.begin(), lanes.begin() + static_cast<std::ptrdiff_t>(row), lane) !=
                lanes.begin() + static_cast<std::ptrdiff_t>(row)) {
            throw std::logic_error("DFlash context append contains an invalid row");
        }
        SequenceState& sequence   = active_sequence(lane);
        const std::uint32_t start = starts[row];
        const std::uint64_t end64 = static_cast<std::uint64_t>(start) + counts[row];
        const std::uint32_t end   = static_cast<std::uint32_t>(end64);
        if (!sequence.kv || text_kv_addresses->bound_row(sequence.kv->text) < 0 ||
            (backend_kv_cache() && (!sequence.kv->backend ||
                                    backend_kv_addresses->bound_row(*sequence.kv->backend) < 0)) ||
            end64 > capacity) {
            throw std::logic_error("DFlash context append is outside retained target storage");
        }
        dflash_host_ingress->context_frontiers[row] =
            checked_i32(start, "DFlash append context frontier");
        dflash_host_ingress->execution_frontiers[row] =
            checked_i32(end, "DFlash append target frontier");
        dflash_host_ingress->dflash_kv_table_rows[row] =
            sequence.kv->backend ? backend_kv_addresses->bound_row(*sequence.kv->backend) : 0;
        dflash_host_ingress->active_lanes[row]            = static_cast<std::int32_t>(lane);
        const StateImageSelectors selectors               = state_selectors(sequence);
        dflash_host_ingress->state_source_slots[row]      = selectors.source;
        dflash_host_ingress->state_destination_slots[row] = selectors.destination;
        // Context append writes only the draft caches. Target execution owns Main KV
        // coverage, including any uncommitted suffix that survives until the round is settled.
        // DFlash2 has only fixed cyclic state; DFlash also grows its Full backend KV here.
        if (sequence.kv->backend) {
            backend_kv_addresses->ensure_mapped_to_tokens(*sequence.kv->backend, end,
                                                          device.stream);
        }
        minimum_count = std::min(minimum_count, counts[row]);
        maximum_count = std::max(maximum_count, counts[row]);
    }

    qwen3_5::DFlashDecodeState& frame = *io.dflash_decode;
    CUDA_CHECK(cudaMemcpyAsync(frame.ingress.data, dflash_host_ingress,
                               sizeof(qwen3_5::DFlashDecodeIngress), cudaMemcpyHostToDevice,
                               device.stream));
    const auto batch                = static_cast<std::int32_t>(lanes.size());
    Tensor active_lane_tensor       = frame.active_lanes.slice(0, 0, batch);
    Tensor state_destination_tensor = frame.state_destination_slots.slice(0, 0, batch);
    Tensor device_starts            = frame.context_frontiers.slice(0, 0, batch);
    Tensor device_ends              = frame.execution_frontiers.slice(0, 0, batch);
    Tensor table_rows               = frame.dflash_kv_table_rows.slice(0, 0, batch);
    Tensor positions                = frame.append_positions.slice(1, 0, batch);
    Tensor device_counts            = frame.append_counts.slice(0, 0, batch);

    work.reset();
    Tensor features =
        work.alloc(DType::BF16, {dimension(parameters.draft->feature_projection.weight.k),
                                 static_cast<std::int32_t>(draft_window + 1U), batch});
    ops::prepare_ragged_prefix(dflash->pending_features, active_lane_tensor, device_starts,
                               device_ends, features, positions, device_counts, device.stream);

    execution::DFlashAppendContext state{{device, parameters, work, state_images->linear(),
                                          replay_records ? &*replay_records : nullptr, io,
                                          prefill_hidden, prefill_chunk, proposal_head,
                                          rope_scaling_factor,
                                          rope_scaling_original_context},
                                         *dflash};
    mark_workspace_usage(workspace_plan.dflash_context);
    execution::dflash_append_context(state, features, positions, device_counts,
                                     state_destination_tensor, table_rows,
                                     {minimum_count, maximum_count});
}

void ProgramImpl::validate_licensed_tokens(std::span<const TokenId> tokens) const {
    for (const TokenId token : tokens) {
        if (token < 0 || token >= dimension(parameters.model.resources().public_token_count)) {
            throw std::runtime_error("target returned a token outside the public token domain");
        }
    }
}

runtime::BatchedGeneratedRound
ProgramImpl::decode_ordinary_batch(std::span<const std::uint32_t> lanes,
                                   std::span<const runtime::RoundBudget> budgets,
                                   runtime::ExecutionTiming* failed_timing) {
    nvtx::ScopedRange round_range(nvtx::Name::DecodeOrdinaryRound, nvtx::Category::Decode,
                                  static_cast<std::uint64_t>(lanes.size()));
    runtime::ExecutionTimingRecorder timing(runtime::ExecutionTimingPhase::Submit, failed_timing);
    if (speculative_backend != SpeculativeBackend::None) {
        throw std::logic_error("ordinary batch execution requires the ordinary backend");
    }
    if (lanes.empty() || lanes.size() > max_concurrency || budgets.size() != lanes.size()) {
        throw std::invalid_argument("ordinary batch membership is invalid");
    }

    std::uint32_t maximum_frontier = 0;
    for (std::size_t row = 0; row < lanes.size(); ++row) {
        const std::uint32_t lane = lanes[row];
        if (lane >= max_concurrency ||
            std::find(lanes.begin(), lanes.begin() + static_cast<std::ptrdiff_t>(row), lane) !=
                lanes.begin() + static_cast<std::ptrdiff_t>(row)) {
            throw std::invalid_argument("ordinary batch contains an invalid or duplicate lane");
        }
        const SequenceState& sequence = active_sequence(lane);
        const RequestControl& request = requests[lane];
        if (request.lifecycle != Lifecycle::Active ||
            budgets[row].generated_tokens_remaining == 0 || !sequence.kv ||
            text_kv_addresses->bound_row(sequence.kv->text) < 0 ||
            sequence.execution_frontier >= capacity ||
            sequence.ledger_frontier != sequence.execution_frontier + 1 ||
            sequence.ledger.size() != sequence.ledger_frontier ||
            sequence.prefix_identity.size() != sequence.ledger_frontier ||
            sequence.prefix_digests.size() != sequence.ledger_frontier) {
            throw std::logic_error("ordinary batch row is not decode-ready");
        }
        maximum_frontier = std::max(maximum_frontier, sequence.execution_frontier);
    }

    // See the note in prefill.cpp: N1 is not fixed by waiting on the compute side.
    // KV content fingerprint (NINFER_KV_PROBE=1). Samples the first 4 KB of a physical page per plane
    // and prints a digest of those bytes.
    //
    // EXTENDED (2026-09-24): not the frontier page only. The canary sits INSIDE the private document,
    // thousands of tokens before the frontier, so sampling only `pages-1` could never cover it -- which
    // is why the first version's "no cross-lane page sharing" was a bounded negative about one page per
    // lane, not about the region the evidence points at. It now samples at three offsets through the
    // lane's mapped range (1/3, 2/3, last) and prints the logical index beside the physical page.
    // Reading rule, pre-committed: a digest shared by two lanes at *different* logical positions is
    // shared bytes; a shared page at the *same* position is the shared prefix and legitimate.
    //
    // `sum`/`peak` are gone deliberately: they decoded an NVFP4 KV buffer as float words and printed
    // `-nan` / 3.3e38, values that invite a reader to believe the probe measured magnitudes it did not.
    if (std::getenv("NINFER_KV_PROBE") != nullptr) {
        const DeviceKVPagePool& pool = text_kv_pages->physical_pool();
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            const SequenceState& state = active_sequence(lanes[row]);
            if (!state.kv) { continue; }
            const std::uint32_t pages = text_kv_addresses->mapped_pages(state.kv->text);
            if (pages == 0) { continue; }
            // `NINFER_KV_PROBE=full` digests EVERY mapped page of this lane's range once, at its first
            // decode step -- the 3-offset mode covers 3 of ~950 pages per lane, which bounds what its
            // "no cross-lane sharing" negative can claim. Full coverage is affordable exactly once per
            // lane (~950 x 4 KB x planes), so it is gated on `execution_frontier ==
            // admitted_prompt_tokens` (the first decode step) rather than run every step.
            const char* const kv_mode = std::getenv("NINFER_KV_PROBE");
            const bool full_mode      = kv_mode != nullptr && std::strcmp(kv_mode, "full") == 0;
            if (full_mode && state.execution_frontier != state.admitted_prompt_tokens) { continue; }
            std::vector<std::uint32_t> offsets;
            if (full_mode) {
                offsets.resize(pages);
                for (std::uint32_t logical = 0; logical < pages; ++logical) { offsets[logical] = logical; }
            } else {
                offsets = {pages / 3U, (2U * pages) / 3U, pages - 1U};
            }
            for (const std::uint32_t logical : offsets) {
                const std::int32_t phys =
                    text_kv_addresses->physical_page_index(state.kv->text, logical);
                if (phys < 0) { continue; }
                std::uint64_t digest = 1469598103934665603ULL;
                for (std::size_t plane_index = 0; plane_index < pool.plane_count(); ++plane_index) {
                    const Tensor& plane = pool.plane(plane_index);
                    if (plane.data == nullptr || plane.nb[3] <= 0) { continue; }
                    const auto* base = static_cast<const unsigned char*>(plane.data) +
                                       static_cast<std::size_t>(phys) * plane.nb[3];
                    // The WHOLE page stride, not its first 4 KB. The 4 KB version hashed a region
                    // whose bytes did not vary with the page's content: 597 distinct pages came back
                    // with one identical digest, which turned "no cross-lane sharing" into a
                    // statement about the probe rather than about the KV (2026-09-25 00:35).
                    const std::size_t bytes = static_cast<std::size_t>(plane.nb[3]);
                    std::vector<unsigned char> sample(bytes, 0);
                    CUDA_CHECK(cudaMemcpyAsync(sample.data(), base, bytes, cudaMemcpyDeviceToHost,
                                               device.stream));
                    device.synchronize();
                    for (std::size_t i = 0; i < sample.size(); ++i) {
                        digest ^= sample[i];
                        digest *= 1099511628211ULL;
                    }
                }
                std::fprintf(stderr, "[mat-debug] KV-FP lane=%u frontier=%u logical=%u page=%u digest=%llx\n",
                             lanes[row], state.execution_frontier, logical, static_cast<unsigned>(phys),
                             static_cast<unsigned long long>(digest));
            }
        }
        std::fflush(stderr);
    }

    const auto start = Clock::now();
    try {
        std::optional<nvtx::ScopedRange> submit_range;
        submit_range.emplace(nvtx::Name::DecodeOrdinarySubmit, nvtx::Category::Decode,
                             static_cast<std::uint64_t>(lanes.size()));
        DecodeGraphExecutable* executable = nullptr;
        ops::CausalAttentionExecutionEnvelope envelope{maximum_frontier + 1, maximum_frontier + 1};
        if (use_cuda_graph) {
            DecodeGraphProfile& profile =
                select_graph_profile(ordinary_graphs, static_cast<std::uint32_t>(lanes.size()),
                                     maximum_frontier, "ordinary batch");
            executable = &install_graph_profile(ordinary_graphs, profile, "ordinary batch");
            envelope   = {profile.min_execution_frontier + 1, profile.max_execution_frontier + 1};
        }

        for (std::size_t row = 0; row < lanes.size(); ++row) {
            SequenceState& sequence            = active_sequence(lanes[row]);
            const RequestControl& request      = requests[lanes[row]];
            const std::uint32_t frontier       = sequence.execution_frontier;
            ordinary_host_ingress->tokens[row] = sequence.ledger.back();
            ordinary_host_ingress->cache_positions[row] =
                checked_i32(frontier, "ordinary batch position");
            ordinary_host_ingress->rope_positions[row] =
                yarn_scale_position(
                    checked_i32(frontier, "ordinary batch RoPE position") + sequence.rope_delta,
                    rope_scaling_original_context, rope_scaling_factor);
            ordinary_host_ingress->text_kv_table_rows[row] =
                text_kv_addresses->bound_row(sequence.kv->text);
            const StateImageSelectors selectors                 = state_selectors(sequence);
            ordinary_host_ingress->state_source_slots[row]      = selectors.source;
            ordinary_host_ingress->state_destination_slots[row] = selectors.destination;
            // Visible keys for THIS row. HONEST SCOPE (third review): at width 1 this is INERT --
            // the SmallT kernel derives each row's key window from its own `positions` entry
            // (`small_t_k8v4.cuh`, `window = positions[TokenTile-1] + 1`), and the mask only bounds
            // output columns via `absolute_column >= valid_columns[batch]`, which is never true for
            // `frontier + 1`. The earlier justification here ("without a per-row bound a row attends
            // past its own context") was contradicted by the kernel source and by this plan's own
            // cleared-table row. Kept as a guard for width > 1 and as the binding `NINFER_MASK_PROBE`
            // toggles -- whose "decisive" 11:07 reading measured nothing.
            // NINFER_MASK_PROBE=1 was added as a positive control ("bound each row to ONE key").
            // It is NOT one: at width 1 the value reaches the kernel only as
            // `absolute_column >= valid_columns[batch]` with token 0 and column_begin 0, and the
            // producer clamps valid_tokens to TokenTile = 1, so 1 is bit-identical to frontier + 1
            // (fifth review, verified against all four kernel consumers). It is meaningful only
            // for width > 1, to which it is not wired, so it is inert as wired -- and the
            // "decisive control" reading once taken from it measured nothing.
            ordinary_host_ingress->valid_columns[row] =
                diagnostic_control_enabled("NINFER_MASK_PROBE")
                    ? 1
                    : checked_i32(frontier + 1U, "ordinary batch visible keys");
            ordinary_host_ingress->sampling[row]                = request.sampling_host;
            ensure_sequence_kv_mapped(sequence, frontier + 1, 0);
            // Slot provenance (NINFER_SLOT_PROBE=1): record this lane as the writer of its
            // destination slot, and shout if it READS a slot a *different* lane wrote last. The
            // proven symptom is a victim emitting the canary of the request admitted just before
            // it, so a read of another lane's slot is exactly the aliasing to catch.
            // Ledger provenance (NINFER_LEDGER_PROBE=1): what the model is actually fed as this
            // row's input token, and whether the ledger behind it is this sequence's own prompt.
            // Every binding is verified; the ledger's *contents* were never checked, and the
            // symptom is a victim continuing the request admitted immediately before it.
            if (std::getenv("NINFER_LEDGER_PROBE") != nullptr) {
                const std::uint32_t ledger_size =
                    static_cast<std::uint32_t>(sequence.ledger.size());
                std::uint64_t prefix_hash = 1469598103934665603ULL;
                const std::size_t prefix = std::min<std::size_t>(sequence.ledger.size(), 64U);
                for (std::size_t index = 0; index < prefix; ++index) {
                    prefix_hash ^= static_cast<std::uint64_t>(sequence.ledger[index]);
                    prefix_hash *= 1099511628211ULL;
                }
                const bool first_step =
                    sequence.execution_frontier + 2U >= sequence.admitted_prompt_tokens;
                // TAIL hash: the 64 entries ending at `admitted_prompt_tokens` are this lane's OWN
                // document, while the first 64 (`prefix64`) are the shared system prefix and identical
                // for every lane by construction -- which is why a prefix-only hash could never
                // discriminate anything. `tail_mismatch` compares those entries with the same range of
                // the request's own prompt vector: the missing half of "the ledger's contents were
                // never checked" (only its length was).
                std::uint64_t tail_hash     = 1469598103934665603ULL;
                std::uint32_t tail_mismatch = 0;
                std::uint32_t tail_checked  = 0;
                // `tail_compared` (not `tail_checked`) is the denominator a reader needs. In the one
                // run measured, the request's `prefill` record had already been cleared by decode time
                // (`has_prompt=0`), so `prompt_ids` was null and nothing was compared -- while
                // `tail_checked=64 tail_mismatch=0` reads like "64 compared, 0 mismatches", the
                // opposite of the truth. The reset is conditional, not universal: prefill.cpp resets
                // the record unless a prompt-frontier capture is in flight, and commit.cpp resets it
                // on a terminal decision -- so the comparison is live in some configurations and not
                // others, which is exactly why the denominator is printed rather than assumed.
                // Observed in one run whose log did not survive; treat it as a reading until a run
                // with NINFER_LEDGER_PROBE=1 is captured whole.
                std::uint32_t tail_compared = 0;
                const std::uint32_t tail    = std::min<std::uint32_t>(64U, sequence.admitted_prompt_tokens);
                const std::vector<TokenId>* prompt_ids = nullptr;
                if (requests[sequence.lane].prefill) {
                    prompt_ids = &requests[sequence.lane].prefill->prompt.token_ids;
                }
                for (std::uint32_t i = 0; i < tail; ++i) {
                    const std::size_t index = static_cast<std::size_t>(sequence.admitted_prompt_tokens - 1U - i);
                    if (index >= sequence.ledger.size()) { break; }
                    const TokenId entry = sequence.ledger[index];
                    tail_hash ^= static_cast<std::uint64_t>(entry);
                    tail_hash *= 1099511628211ULL;
                    ++tail_checked;
                    if (prompt_ids != nullptr && index < prompt_ids->size()) {
                        ++tail_compared;
                        if ((*prompt_ids)[index] != entry) { ++tail_mismatch; }
                    }
                }
                std::fprintf(stderr,
                             "[mat-debug] LEDGER-FP lane=%u frontier=%u ledger=%u admitted=%u "
                             "input_token=%d prefix64=%llx first_step=%d batch=%zu "
                             "tail64=%llx tail_checked=%u tail_compared=%u tail_mismatch=%u "
                             "has_prompt=%d\n",
                             lanes[row], frontier, ledger_size, sequence.admitted_prompt_tokens,
                             static_cast<int>(ordinary_host_ingress->tokens[row]),
                             static_cast<unsigned long long>(prefix_hash), first_step ? 1 : 0,
                             lanes.size(), static_cast<unsigned long long>(tail_hash), tail_checked,
                             tail_compared, tail_mismatch, prompt_ids != nullptr ? 1 : 0);
                std::fflush(stderr);
            }
            if (std::getenv("NINFER_SLOT_PROBE") != nullptr) {
                ++step_counter;
                const std::int32_t source = ordinary_host_ingress->state_source_slots[row];
                const std::int32_t destination =
                    ordinary_host_ingress->state_destination_slots[row];
                const auto last = slot_last_writer.find(source);
                if (last != slot_last_writer.end() && last->second.first != lanes[row]) {
                    std::fprintf(stderr,
                                 "[mat-debug] SLOT-READ-FOREIGN lane=%u slot=%d written_by_lane=%u "
                                 "at_step=%llu now_step=%llu frontier=%u\n",
                                 lanes[row], source, last->second.first,
                                 static_cast<unsigned long long>(last->second.second),
                                 static_cast<unsigned long long>(step_counter), frontier);
                    std::fflush(stderr);
                }
                if (destination >= 0) {
                    slot_last_writer[destination] = {lanes[row], step_counter};
                }
            }
        }
        // Within-batch state-slot uniqueness (NINFER_MAT_DEBUG=1). The cross-session canary
        // isolates the leak to the state/hidden path (a row bounded to ONE attention key still
        // reproduced another row's canary), so two rows of one batch resolving to the same
        // StateImage slot is the signature to catch -- the between-steps check never sees it.
        if (std::getenv("NINFER_MAT_DEBUG") != nullptr) {
            const auto slot_of = [&](const SequenceState& seq, bool destination) -> std::int32_t {
                const StateImageHandle& handle =
                    destination ? seq.state.write : seq.state.read;
                if (!state_store->valid(handle) ||
                    state_store->residency(handle) == StateReplicaResidency::HostOnly) {
                    return -1;
                }
                return state_store->physical_slot(handle);
            };
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                const SequenceState& mine = active_sequence(lanes[row]);
                std::fprintf(stderr,
                             "[mat-debug] DECODE-ROW row=%zu lane=%u frontier=%u src_slot=%d "
                             "dst_slot=%d batch=%zu\n",
                             row, lanes[row], mine.execution_frontier,
                             slot_of(mine, false), slot_of(mine, true), lanes.size());
                for (std::size_t other = row + 1; other < lanes.size(); ++other) {
                    const SequenceState& theirs = active_sequence(lanes[other]);
                    for (bool dst : {false, true}) {
                        const std::int32_t a = slot_of(mine, dst);
                        const std::int32_t b = slot_of(theirs, dst);
                        if (a >= 0 && a == b) {
                            std::fprintf(stderr,
                                         "[mat-debug] SLOT-SHARE-IN-BATCH lane=%u other_lane=%u "
                                         "slot=%d role=%s batch=%zu\n",
                                         lanes[row], lanes[other], a, dst ? "write" : "read",
                                         lanes.size());
                        }
                    }
            // Physical KV page exclusivity inside one decode batch. Bindings (rows, slots) are
            // exclusive, so the last place two lanes can meet is the physical page behind a
            // logical column. A page mapped by two lanes is only legitimate where their *tokens*
            // agree (a read-only shared prefix); where the tokens differ the page cannot hold
            // both sequences, and one lane decodes with the other's content.
            {
                constexpr std::uint32_t kColumnsPerPage = 64;
                std::unordered_map<std::int32_t,
                                   std::vector<std::pair<std::uint32_t, std::uint32_t>>>
                    by_physical;
                for (std::size_t row = 0; row < lanes.size(); ++row) {
                    const SequenceState& state = active_sequence(lanes[row]);
                    if (!state.kv) { continue; }
                    const std::uint32_t pages = text_kv_addresses->mapped_pages(state.kv->text);
                    for (std::uint32_t page = 0; page < pages; ++page) {
                        const std::int32_t physical =
                            text_kv_addresses->physical_page_index(state.kv->text, page);
                        if (physical < 0) { continue; }
                        by_physical[physical].emplace_back(lanes[row], page);
                    }
                }
                std::uint32_t reported = 0;
                for (const auto& [physical, owners] : by_physical) {
                    if (owners.size() < 2 || reported >= 8) { continue; }
                    for (std::size_t i = 0; i < owners.size(); ++i) {
                        for (std::size_t j = i + 1; j < owners.size(); ++j) {
                            const SequenceState& left  = active_sequence(owners[i].first);
                            const SequenceState& right = active_sequence(owners[j].first);
                            const std::uint32_t left_begin  = owners[i].second * kColumnsPerPage;
                            const std::uint32_t right_begin = owners[j].second * kColumnsPerPage;
                            const std::uint32_t begin = std::max(left_begin, right_begin);
                            const std::uint32_t end =
                                std::min(left_begin + kColumnsPerPage, right_begin + kColumnsPerPage);
                            for (std::uint32_t column = begin; column < end; ++column) {
                                const std::uint32_t left_column  = column - left_begin;
                                const std::uint32_t right_column = column - right_begin;
                                if (left_column >= left.ledger.size() ||
                                    right_column >= right.ledger.size()) {
                                    break;
                                }
                                if (left.ledger[left_column] == right.ledger[right_column]) {
                                    continue;
                                }
                                std::fprintf(
                                    stderr,
                                    "[mat-debug] PAGE-ALIAS-MISMATCH phys=%d lane=%u page=%u "
                                    "column=%u token=%d other_lane=%u other_column=%u token=%d "
                                    "batch=%zu\n",
                                    physical, owners[i].first, owners[i].second, left_column,
                                    static_cast<int>(left.ledger[left_column]), owners[j].first,
                                    right_column, static_cast<int>(right.ledger[right_column]),
                                    lanes.size());
                                ++reported;
                                break;
                            }
                            if (reported >= 8) { break; }
                        }
                    }
                }
                std::fflush(stderr);
            }
                }
            }
            // Device block table vs host bookkeeping. Everything host-side is verified clean, so
            // the last link is the published Device row the kernels actually read: if it names a
            // page other than the address's own, a lane reads another sequence's KV.
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                const SequenceState& state = active_sequence(lanes[row]);
                if (!state.kv) { continue; }
                const std::uint32_t pages = text_kv_addresses->mapped_pages(state.kv->text);
                if (pages == 0) { continue; }
                const Tensor table = text_kv_addresses->execution_table(state.kv->text);
                std::vector<std::int32_t> host_table(pages, -1);
                CUDA_CHECK(cudaMemcpyAsync(host_table.data(), table.data,
                                           host_table.size() * sizeof(std::int32_t),
                                           cudaMemcpyDeviceToHost, device.stream));
                device.synchronize();
                std::uint32_t mismatches = 0;
                for (std::uint32_t page = 0; page < pages; ++page) {
                    const std::int32_t expected =
                        text_kv_addresses->physical_page_index(state.kv->text, page);
                    if (host_table[page] == expected) { continue; }
                    if (mismatches < 4) {
                        std::fprintf(stderr,
                                     "[mat-debug] TABLE-MISMATCH lane=%u page=%u device=%d "
                                     "host=%d batch=%zu\n",
                                     lanes[row], page, host_table[page], expected, lanes.size());
                    }
                    ++mismatches;
                }
                if (mismatches != 0) {
                    std::fprintf(stderr,
                                 "[mat-debug] TABLE-MISMATCH-COUNT lane=%u mismatches=%u of %u "
                                 "pages\n",
                                 lanes[row], mismatches, pages);
                }
            }
            // Writable-page exclusivity. A lane's own pages may be shared only as read-only
            // aliases of a shared prefix; a page that another *active* address still references
            // while this lane writes it is a page two sequences write, which is the one way a
            // lane can read content its own tokens do not describe.
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                const SequenceState& state = active_sequence(lanes[row]);
                if (!state.kv) { continue; }
                const std::uint32_t pages = text_kv_addresses->mapped_pages(state.kv->text);
                std::uint32_t shared_writable = 0;
                std::uint32_t seen            = 0;
                for (std::uint32_t page = 0; page < pages; ++page) {
                    const LogicalKVPageHandle logical =
                        text_kv_addresses->logical_page(state.kv->text, page);
                    if (!text_kv_pages->valid(logical)) { continue; }
                    const std::uint32_t active_refs = text_kv_pages->active_address_references(logical);
                    if (active_refs <= 1) { continue; }
                    if (text_kv_pages->writer_references(logical) == 0) { continue; }
                    ++shared_writable;
                    if (seen < 4) {
                        std::fprintf(stderr,
                                     "[mat-debug] PAGE-SHARED-WRITER lane=%u page=%u refs=%u "
                                     "writers=%u protected_cols=%u batch=%zu\n",
                                     lanes[row], page, active_refs,
                                     static_cast<unsigned>(text_kv_pages->writer_references(logical)),
                                     text_kv_pages->protected_columns(logical), lanes.size());
                        ++seen;
                    }
                }
                // Pages this lane WRITES that are also referenced by a NON-ACTIVE owner (a
                // catalogued checkpoint of some other, possibly released, request). The earlier
                // exclusivity audit counted only active references, so a checkpoint's pages were
                // invisible to it -- and writing them silently changes that checkpoint's content.
                std::uint32_t checkpoint_writable = 0;
                std::uint32_t shown                = 0;
                for (std::uint32_t page = 0; page < pages; ++page) {
                    const LogicalKVPageHandle logical =
                        text_kv_addresses->logical_page(state.kv->text, page);
                    if (!text_kv_pages->valid(logical)) { continue; }
                    if (text_kv_pages->writer_references(logical) == 0) { continue; }
                    const std::uint32_t total  = text_kv_pages->address_references(logical);
                    const std::uint32_t active = text_kv_pages->active_address_references(logical);
                    if (total <= active) { continue; }
                    ++checkpoint_writable;
                    if (shown < 4) {
                        std::fprintf(stderr,
                                     "[mat-debug] PAGE-CHECKPOINT-SHARED lane=%u page=%u refs=%u "
                                     "active=%u protected_cols=%u batch=%zu\n",
                                     lanes[row], page, total, active,
                                     text_kv_pages->protected_columns(logical), lanes.size());
                        ++shown;
                    }
                }
                if (checkpoint_writable != 0) {
                    std::fprintf(stderr,
                                 "[mat-debug] PAGE-CHECKPOINT-SHARED-COUNT lane=%u pages=%u of %u\n",
                                 lanes[row], checkpoint_writable, pages);
                }
                if (shared_writable != 0) {
                    std::fprintf(stderr,
                                 "[mat-debug] PAGE-SHARED-WRITER-COUNT lane=%u pages=%u of %u\n",
                                 lanes[row], shared_writable, pages);
                }
            }
            std::fflush(stderr);
        }

        if (std::getenv("NINFER_INGRESS_PROBE") != nullptr && !ingress_shadow) {
            ingress_shadow.emplace(sizeof(qwen3_5::OrdinaryDecodeIngress));
        }
        execution::OrdinaryBatchContext schedule_state{
            {device, parameters, work, state_images->linear(),
             replay_records ? &*replay_records : nullptr, io, prefill_hidden, prefill_chunk,
             proposal_head, rope_scaling_factor, rope_scaling_original_context},
            decoder->text_kv,
            *io.ordinary,
            *ordinary_host_ingress,
            *ordinary_host_egress,
            state_images->continuation_hidden_store(),
            ingress_shadow ? ingress_shadow->data() : nullptr};

        mark_workspace_usage(workspace_plan.ordinary_round);
        execution::ordinary_decode_batch(schedule_state, static_cast<std::int32_t>(lanes.size()),
                                         envelope, executable);
        submit_range.reset();
        timing.begin_wait();
        {
            nvtx::ScopedRange wait_range(nvtx::Name::DecodeOrdinaryWait, nvtx::Category::Control,
                                         static_cast<std::uint64_t>(lanes.size()));
            device.synchronize();
        }
        timing.end_wait();

        // Per-row sampling gather audit (NINFER_SAMPLE_PROBE=1). Every other per-row surface of
        // this batch has been audited and cleared (ingress row->lane mapping, KV table rows,
        // state slots, the attention mask), so the last unverified per-row step is the gather
        // that turns the batch logits into each row's committed token. This copies the batch
        // logits back once and compares each row's argmax with the token the engine committed
        // for that row: a row whose committed token is another row's argmax means the gather
        // (or its stride) is wrong, which at temperature 0 reproduces another session verbatim.
        if (std::getenv("NINFER_SAMPLE_PROBE") != nullptr) {
            // Reuse-equivalence trace: the token each row commits, with the row's own frontier.
            // At temperature 0 the first token of a turn is a function of the prompt alone, so a
            // run with prefix reuse disabled and a run with it enabled must produce the same
            // token trace for the same turn. A difference is a *provable* statement that the
            // reuse path is not content-equivalent, independent of any hypothesis about why.
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                std::fprintf(stderr,
                             "[mat-debug] SAMPLE-TRACE lane=%u owner=%llx frontier=%u token=%d\n",
                             lanes[row],
                             static_cast<unsigned long long>(
                                 active_sequence(lanes[row]).session_key_hash),
                             active_sequence(lanes[row]).execution_frontier,
                             ordinary_host_egress->sampled_tokens[row]);
            }
            std::fflush(stderr);
        }
        if (std::getenv("NINFER_SAMPLE_PROBE") != nullptr) {
            const std::int32_t vocab = static_cast<std::int32_t>(
                parameters.model.resources().public_token_count);
            // ops::sample's contract is "contiguous BF16 [physical_rows,B]"; ne[0] is the fast
            // axis and ne[0] itself is padded above the token domain, so a row's vocabulary
            // starts at row*ne[0] (using the padded width, not the token domain).
            const std::int32_t rows = io.ordinary->logits.ne[0];
            std::vector<std::uint16_t> host_logits(
                static_cast<std::size_t>(rows) * lanes.size(), 0);
            CUDA_CHECK(cudaMemcpyAsync(host_logits.data(), io.ordinary->logits.data,
                                       host_logits.size() * sizeof(std::uint16_t),
                                       cudaMemcpyDeviceToHost, device.stream));
            device.synchronize();
            std::fprintf(stderr,
                         "[mat-debug] SAMPLE-LAYOUT ne0=%d ne1=%d batch=%zu vocab=%d\n", rows,
                         static_cast<std::int32_t>(io.ordinary->logits.ne[1]), lanes.size(), vocab);
            const auto to_float = [](std::uint16_t bits) {
                const std::uint32_t wide = static_cast<std::uint32_t>(bits) << 16U;
                float value              = 0.0F;
                std::memcpy(&value, &wide, sizeof(value));
                return value;
            };
            std::vector<std::int32_t> row_argmax(lanes.size(), -1);
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                const std::uint16_t* column = host_logits.data() + row * rows;
                float best                  = -std::numeric_limits<float>::infinity();
                for (std::int32_t token = 0; token < vocab; ++token) {
                    const float value = to_float(column[token]);
                    if (value > best) {
                        best            = value;
                        row_argmax[row] = token;
                    }
                }
            }
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                const std::int32_t committed = ordinary_host_egress->sampled_tokens[row];
                std::int32_t borrowed        = -1;
                for (std::size_t other = 0; other < lanes.size(); ++other) {
                    if (other != row && row_argmax[other] == committed) {
                        borrowed = static_cast<std::int32_t>(other);
                    }
                }
                const ops::SamplingConfig& config = ordinary_host_ingress->sampling[row];
                // Full-row fingerprint of the (fully written) logits plus the top-3: at
                // temperature 0 the committed token is the argmax, so this row's distribution is
                // exactly what decides the next token. Comparing it concurrent vs serialized for
                // the same prompt and step is a clean divergence oracle -- no stale-byte caveat,
                // because the logits are rewritten from scratch every step.
                std::uint64_t row_hash = 1469598103934665603ULL;
                std::int32_t top[3]    = {-1, -1, -1};
                float top_value[3]     = {-std::numeric_limits<float>::infinity(),
                                          -std::numeric_limits<float>::infinity(),
                                          -std::numeric_limits<float>::infinity()};
                {
                    const std::uint16_t* column = host_logits.data() + row * rows;
                    for (std::int32_t token = 0; token < rows; ++token) {
                        row_hash ^= column[token];
                        row_hash *= 1099511628211ULL;
                        if (token >= vocab) { continue; }
                        const float value = to_float(column[token]);
                        for (int slot = 0; slot < 3; ++slot) {
                            if (value > top_value[slot]) {
                                for (int move = 2; move > slot; --move) {
                                    top_value[move] = top_value[move - 1];
                                    top[move]      = top[move - 1];
                                }
                                top_value[slot] = value;
                                top[slot]       = token;
                                break;
                            }
                        }
                    }
                }
                std::fprintf(stderr,
                             "[mat-debug] SAMPLE-ROW row=%zu lane=%u committed=%d own_argmax=%d "
                             "borrowed_from_row=%d temp=%g top_k=%d top_p=%g batch=%zu\n",
                             row, lanes[row], committed, row_argmax[row], borrowed,
                             static_cast<double>(config.temperature), config.top_k,
                             static_cast<double>(config.top_p), lanes.size());
                std::fprintf(stderr,
                             "[mat-debug] SAMPLE-TOP row=%zu lane=%u frontier=%u hash=%llx "
                             "top=%d:%g %d:%g %d:%g\n",
                             row, lanes[row], active_sequence(lanes[row]).execution_frontier,
                             static_cast<unsigned long long>(row_hash), top[0],
                             static_cast<double>(top_value[0]), top[1],
                             static_cast<double>(top_value[1]), top[2],
                             static_cast<double>(top_value[2]));
            }
            std::fflush(stderr);
        }

        // Device-vs-host ingress comparison (NINFER_INGRESS_PROBE=1). The step's ingress is one
        // cudaMemcpyAsync of the whole host struct, and that copy lives *inside* the captured
        // decode graph -- so the device reads the host image at replay time while the host is free
        // to rewrite it for the next step. A device copy that disagrees with the host image means a
        // row was fed another step's (and possibly another lane's) tokens, slots and positions,
        // which no host-side print can see.
        if (std::getenv("NINFER_INGRESS_PROBE") != nullptr && ingress_shadow) {
            qwen3_5::OrdinaryDecodeIngress device_ingress{};
            CUDA_CHECK(cudaMemcpyAsync(&device_ingress, ingress_shadow->data(),
                                       sizeof(device_ingress), cudaMemcpyDeviceToHost,
                                       device.stream));
            device.synchronize();
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                const std::int32_t host_token  = ordinary_host_ingress->tokens[row];
                const std::int32_t host_row    = ordinary_host_ingress->text_kv_table_rows[row];
                const std::int32_t host_source = ordinary_host_ingress->state_source_slots[row];
                const std::int32_t dev_token   = device_ingress.tokens[row];
                const std::int32_t dev_row     = device_ingress.text_kv_table_rows[row];
                const std::int32_t dev_source  = device_ingress.state_source_slots[row];
                const bool agree = host_token == dev_token && host_row == dev_row &&
                                   host_source == dev_source;
                std::fprintf(stderr,
                             "[mat-debug] INGRESS-FP lane=%u agree=%d host(tok=%d row=%d src=%d) "
                             "device(tok=%d row=%d src=%d)\n",
                             lanes[row], agree ? 1 : 0, host_token, host_row, host_source, dev_token,
                             dev_row, dev_source);
            }
            std::fflush(stderr);
        }

        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        // State-write integrity (NINFER_STATE_PROBE=1). The serialized control run is clean while
        // the concurrent one corrupts a lane's own second step, so the suspicion is what a
        // multi-row decode step writes into the per-row linear-attention destination slot. Print
        // a checksum per row per layer region; the same step in the serialized run must produce
        // the same checksums for the same lane.
        if (std::getenv("NINFER_STATE_PROBE") != nullptr) {
            LinearAttentionStatePool& pool = state_images->linear();
            for (std::size_t row = 0; row < lanes.size(); ++row) {
                const SequenceState& state = active_sequence(lanes[row]);
                const StateImageSelectors selectors = state_selectors(state);
                if (selectors.destination < 0) { continue; }
                std::uint64_t conv_digest = 1469598103934665603ULL;
                std::uint64_t rec_digest  = 1469598103934665603ULL;

                std::uint64_t rec_tail = 0;
                double rec_value_sum = 0.0, rec_value_peak = 0.0;
                for (std::uint32_t layer = 0; layer < pool.layer_count(); ++layer) {
                    // Oracle fix: the conv window is written at column `pos % 3`, so hashing its
                    // first bytes samples uninitialized/stale memory and makes ANY two runs differ
                    // -- which is what made the determinism test flap (identical pairs, then 170
                    // differing). Digest only the recurrent matrix, which the kernel writes in
                    // full every step, and sample its LAST bytes so the offset cannot sit in an
                    // untouched tail.
                    const Tensor rec = pool.recurrent_slot(layer, selectors.destination);
                    const std::size_t rec_rows = std::min<std::size_t>(
                        static_cast<std::size_t>(rec.bytes()), static_cast<std::size_t>(4096));
                    const std::size_t rec_offset =
                        static_cast<std::size_t>(rec.bytes()) - rec_rows;
                    std::vector<unsigned char> buffer(rec_rows, 0);
                    CUDA_CHECK(cudaMemcpyAsync(buffer.data(),
                                               static_cast<const unsigned char*>(rec.data) +
                                                   rec_offset,
                                               rec_rows, cudaMemcpyDeviceToHost, device.stream));
                    device.synchronize();
                    // Magnitude, not a digest: an FNV over raw bytes is all-or-nothing, so it
                    // cannot tell "the same state to within float rounding" from "a different
                    // state". Reduction order depends on batch composition (split counts), which
                    // varies with timing, so bit-level differences across runs are expected and
                    // meaningless; a relative magnitude difference is not.
                    // The recurrent matrix is FP32 (`validate_state_tensor(recurrent_[layer],
                    // DType::FP32, ...)`), not BF16 -- decoding it as bf16 produced the 1e38/NaN
                    // garbage the first version of this print reported.
                    double rec_sum = 0.0;
                    float rec_peak = 0.0F;
                    if (buffer.size() % sizeof(float) == 0) {
                        for (std::size_t index = 0; index + sizeof(float) <= buffer.size();
                             index += sizeof(float)) {
                            float value = 0.0F;
                            std::memcpy(&value, buffer.data() + index, sizeof(value));
                            rec_sum += static_cast<double>(value);
                            rec_peak = std::max(rec_peak, std::abs(value));
                        }
                    }
                    // A tail digest as well, for a *cross-lane* question the magnitudes cannot answer:
                    // a hash is all-or-nothing, which is wrong for a cross-run comparison (batch
                    // composition changes the low bits), but exactly right for "do two lanes hold the
                    // same bytes?" -- two lanes' states differ legitimately, so a collision between
                    // them is shared content, and a collision is what this prints.
                    std::uint64_t tail_digest = 1469598103934665603ULL;
                    for (std::size_t index = 0; index < buffer.size(); ++index) {
                        tail_digest ^= buffer[index];
                        tail_digest *= 1099511628211ULL;
                    }
                    rec_digest = 0;
                    rec_value_sum = rec_sum;
                    rec_value_peak = static_cast<double>(rec_peak);
                    rec_tail       = tail_digest;
                    conv_digest = 0;
                }
                std::fprintf(stderr,
                             "[mat-debug] STATE-SLOT lane=%u frontier=%u src=%d dst=%d "
                             "conv=%llx rec=%llx\n",
                             lanes[row], state.execution_frontier, selectors.source,
                             selectors.destination,
                             static_cast<unsigned long long>(conv_digest),
                             static_cast<unsigned long long>(rec_digest));
                std::fprintf(stderr,
                             "[mat-debug] STATE-MAG lane=%u frontier=%u sum=%.6f peak=%.6f "
                             "rec_tail=%llx\n",
                             lanes[row], state.execution_frontier, rec_value_sum, rec_value_peak,
                             static_cast<unsigned long long>(rec_tail));
            }
            std::fflush(stderr);
        }
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            SequenceState& sequence    = active_sequence(lanes[row]);
            RequestControl& request    = requests[lanes[row]];
            const std::uint32_t base_E = sequence.execution_frontier;
            const std::uint32_t base_S = sequence.ledger_frontier;
            const TokenId token        = ordinary_host_egress->sampled_tokens[row];
            validate_licensed_tokens(std::span<const TokenId>(&token, 1));
            sequence.text_kv_valid = base_E + 1;
            commit_sequence_kv(sequence, sequence.text_kv_valid, 0);
            sequence.tail_hidden_valid = true;
            sequence.ledger.push_back(token);
            sequence.prefix_identity.append_generated(1, sequence.rope_delta);
            sequence.prefix_digests.append_generated(std::span<const TokenId>(&token, 1),
                                                     sequence.rope_delta);
            request.pending   = PendingCandidate{.kind          = PendingKind::Ordinary,
                                                 .base_E        = base_E,
                                                 .base_S        = base_S,
                                                 .prompt_tokens = 0,
                                                 .produced      = 1};
            request.lifecycle = Lifecycle::Pending;
            request.timings.decode_seconds += seconds;
        }
        return runtime::BatchedGeneratedRound{
            .tokens =
                std::span<const TokenId>(ordinary_host_egress->sampled_tokens.data(), lanes.size()),
            .timing = timing.finish(),
        };
    } catch (...) {
        timing.begin_wait();
        try {
            nvtx::ScopedRange wait_range(nvtx::Name::DecodeOrdinaryWait, nvtx::Category::Control,
                                         static_cast<std::uint64_t>(lanes.size()));
            device.synchronize();
        } catch (...) {}
        timing.end_wait();
        clear_execution_failure_lanes(lanes);
        throw;
    }
}

runtime::BatchedGeneratedRound
ProgramImpl::decode_mtp_batch(std::span<const std::uint32_t> lanes,
                              std::span<const runtime::RoundBudget> budgets,
                              runtime::ExecutionTiming* failed_timing) {
    nvtx::ScopedRange round_range(nvtx::Name::DecodeMtpRound, nvtx::Category::Mtp,
                                  static_cast<std::uint64_t>(lanes.size()));
    runtime::ExecutionTimingRecorder timing(runtime::ExecutionTimingPhase::Submit, failed_timing);
    if (speculative_backend != SpeculativeBackend::Mtp || !io.mtp_decode ||
        decoder->mtp_cache() == nullptr) {
        throw std::logic_error("MTP batch execution requires the MTP backend");
    }
    if (lanes.empty() || lanes.size() > max_concurrency || budgets.size() != lanes.size()) {
        throw std::invalid_argument("MTP batch membership is invalid");
    }

    const std::uint32_t width      = draft_window + 1;
    std::uint32_t maximum_frontier = 0;
    for (std::size_t row = 0; row < lanes.size(); ++row) {
        const std::uint32_t lane = lanes[row];
        if (lane >= max_concurrency ||
            std::find(lanes.begin(), lanes.begin() + static_cast<std::ptrdiff_t>(row), lane) !=
                lanes.begin() + static_cast<std::ptrdiff_t>(row)) {
            throw std::invalid_argument("MTP batch contains an invalid or duplicate lane");
        }
        const SequenceState& sequence = active_sequence(lane);
        const RequestControl& request = requests[lane];
        if (request.lifecycle != Lifecycle::Active ||
            budgets[row].generated_tokens_remaining == 0 || !sequence.kv || !sequence.kv->backend ||
            text_kv_addresses->bound_row(sequence.kv->text) < 0 ||
            backend_kv_addresses->bound_row(*sequence.kv->backend) < 0 ||
            sequence.execution_frontier >= capacity ||
            sequence.mtp_kv_valid != sequence.execution_frontier ||
            sequence.ledger_frontier != sequence.execution_frontier + 1 ||
            sequence.ledger.size() != sequence.ledger_frontier ||
            sequence.prefix_identity.size() != sequence.ledger_frontier ||
            sequence.prefix_digests.size() != sequence.ledger_frontier ||
            sequence.mtp_draft_count > draft_window) {
            throw std::logic_error("MTP batch row is not decode-ready");
        }
        maximum_frontier = std::max(maximum_frontier, sequence.execution_frontier);
    }

    const auto started = Clock::now();
    try {
        std::optional<nvtx::ScopedRange> submit_range;
        submit_range.emplace(nvtx::Name::DecodeMtpSubmit, nvtx::Category::Mtp,
                             static_cast<std::uint64_t>(lanes.size()));
        DecodeGraphExecutable* executable = nullptr;
        execution::MtpCausalAttentionEnvelopes envelopes =
            mtp_causal_attention_envelopes(maximum_frontier, draft_window, capacity);
        if (use_cuda_graph) {
            DecodeGraphProfile& profile =
                select_graph_profile(mtp_graphs, static_cast<std::uint32_t>(lanes.size()),
                                     maximum_frontier, "MTP batch");
            executable = &install_graph_profile(mtp_graphs, profile, "MTP batch");
            envelopes = mtp_causal_attention_envelopes(profile.max_execution_frontier, draft_window,
                                                       capacity);
        }

        for (std::size_t row = 0; row < lanes.size(); ++row) {
            SequenceState& sequence           = active_sequence(lanes[row]);
            const RequestControl& request     = requests[lanes[row]];
            const std::uint32_t frontier      = sequence.execution_frontier;
            const std::uint32_t max_by_budget = budgets[row].generated_tokens_remaining > 1
                                                    ? budgets[row].generated_tokens_remaining - 1
                                                    : 0;
            const std::uint32_t extent =
                std::min({sequence.mtp_draft_count, draft_window, max_by_budget,
                          capacity - sequence.execution_frontier - 1});
            mtp_host_ingress->anchors[row]        = sequence.ledger.back();
            mtp_host_ingress->base_frontiers[row] = checked_i32(frontier, "MTP batch frontier");
            mtp_host_ingress->remaining_budgets[row] =
                checked_i32(budgets[row].generated_tokens_remaining, "MTP batch remaining budget");
            mtp_host_ingress->current_extents[row]      = static_cast<std::int32_t>(extent);
            mtp_host_ingress->target_valid_columns[row] = static_cast<std::int32_t>(extent + 1);
            for (std::uint32_t j = 0; j < draft_window; ++j) {
                mtp_host_ingress->current_drafts[row * draft_window + j] =
                    j < extent ? sequence.mtp_drafts[j] : sequence.ledger.back();
            }
            for (std::uint32_t j = 0; j < width; ++j) {
                const std::uint32_t position = frontier + std::min(j, extent);
                mtp_host_ingress->target_rope_positions[row * width + j] =
                    yarn_scale_position(
                        checked_i32(position, "MTP batch RoPE position") + sequence.rope_delta,
                        rope_scaling_original_context, rope_scaling_factor);
            }
            mtp_host_ingress->text_kv_table_rows[row] =
                text_kv_addresses->bound_row(sequence.kv->text);
            mtp_host_ingress->mtp_kv_table_rows[row] =
                backend_kv_addresses->bound_row(*sequence.kv->backend);
            const StateImageSelectors selectors            = state_selectors(sequence);
            mtp_host_ingress->state_source_slots[row]      = selectors.source;
            mtp_host_ingress->state_destination_slots[row] = selectors.destination;
            mtp_host_ingress->rope_deltas[row]             = sequence.rope_delta;
            mtp_host_ingress->sampling[row]                = request.sampling_host;
            ensure_sequence_kv_mapped(sequence, frontier + extent + 1,
                                      std::min(capacity, frontier + extent + draft_window));
        }

        execution::MtpBatchContext schedule_state{{device, parameters, work, state_images->linear(),
                                                   replay_records ? &*replay_records : nullptr, io,
                                                   prefill_hidden, prefill_chunk, proposal_head,
                                                   rope_scaling_factor,
                                                   rope_scaling_original_context},
                                                  decoder->text_kv,
                                                  *decoder->mtp_cache(),
                                                  *io.mtp_decode,
                                                  *mtp_host_ingress,
                                                  *mtp_host_egress,
                                                  state_images->continuation_hidden_store()};

        mark_workspace_usage(workspace_plan.mtp_round);
        execution::mtp_decode_batch(schedule_state, static_cast<std::int32_t>(lanes.size()),
                                    draft_window, envelopes, executable);
        submit_range.reset();
        timing.begin_wait();
        {
            nvtx::ScopedRange wait_range(nvtx::Name::DecodeMtpWait, nvtx::Category::Control,
                                         static_cast<std::uint64_t>(lanes.size()));
            device.synchronize();
        }
        timing.end_wait();

        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            SequenceState& sequence       = active_sequence(lanes[row]);
            RequestControl& request       = requests[lanes[row]];
            const std::uint32_t base_E    = sequence.execution_frontier;
            const std::uint32_t base_S    = sequence.ledger_frontier;
            const std::int32_t count_i    = mtp_host_egress->licensed_counts[row];
            const std::int32_t accepted_i = mtp_host_egress->accepted_drafts[row];
            const std::int32_t next_i     = mtp_host_egress->next_extents[row];
            if (count_i <= 0 || count_i > static_cast<std::int32_t>(width) || accepted_i < 0 ||
                accepted_i + 1 != count_i || next_i < 0 ||
                next_i > static_cast<std::int32_t>(draft_window) ||
                static_cast<std::uint32_t>(count_i) > budgets[row].generated_tokens_remaining ||
                static_cast<std::uint64_t>(base_E) + static_cast<std::uint32_t>(count_i) >
                    capacity) {
                throw std::runtime_error("MTP batch returned invalid row metadata");
            }
            const std::span<const TokenId> row_tokens(mtp_host_egress->licensed_tokens.data() +
                                                          row * width,
                                                      static_cast<std::size_t>(count_i));
            validate_licensed_tokens(row_tokens);
            const std::uint32_t pcur =
                static_cast<std::uint32_t>(mtp_host_ingress->current_extents[row]);
            if (pcur == 0) {
                request.speculative_stats.fallback_steps += 1;
            } else {
                request.speculative_stats.rounds += 1;
                request.speculative_stats.drafted_tokens += pcur;
                request.speculative_stats.accepted_tokens += static_cast<std::uint32_t>(accepted_i);
                for (std::int32_t i = 0; i < accepted_i; ++i) {
                    request.speculative_stats.accepted_per_position[static_cast<std::size_t>(i)] +=
                        1;
                }
            }
            request.pending = PendingCandidate{
                .kind          = PendingKind::Speculative,
                .base_E        = base_E,
                .base_S        = base_S,
                .prompt_tokens = 0,
                .produced      = static_cast<std::uint32_t>(count_i),
            };
            request.lifecycle = Lifecycle::Pending;
            request.timings.decode_seconds += seconds;
        }
        return runtime::BatchedGeneratedRound{
            .tokens     = std::span<const TokenId>(mtp_host_egress->licensed_tokens.data(),
                                                   lanes.size() * width),
            .row_counts = std::span<const std::int32_t>(mtp_host_egress->licensed_counts.data(),
                                                        lanes.size()),
            .row_stride = width,
            .timing     = timing.finish(),
        };
    } catch (...) {
        timing.begin_wait();
        try {
            nvtx::ScopedRange wait_range(nvtx::Name::DecodeMtpWait, nvtx::Category::Control,
                                         static_cast<std::uint64_t>(lanes.size()));
            device.synchronize();
        } catch (...) {}
        timing.end_wait();
        clear_execution_failure_lanes(lanes);
        throw;
    }
}

runtime::BatchedGeneratedRound
ProgramImpl::decode_dflash_batch(std::span<const std::uint32_t> lanes,
                                 std::span<const runtime::RoundBudget> budgets,
                                 runtime::ExecutionTiming* failed_timing) {
    nvtx::ScopedRange round_range(nvtx::Name::DecodeDFlashRound, nvtx::Category::DFlash,
                                  static_cast<std::uint64_t>(lanes.size()));
    runtime::ExecutionTimingRecorder timing(runtime::ExecutionTimingPhase::Submit, failed_timing);
    if (!is_masked_draft_backend(speculative_backend) || !io.dflash_decode || !dflash) {
        throw std::logic_error("DFlash batch execution requires the DFlash backend");
    }
    if (lanes.empty() || lanes.size() > max_concurrency || budgets.size() != lanes.size()) {
        throw std::invalid_argument("DFlash batch membership is invalid");
    }

    const std::uint32_t width           = draft_window + 1U;
    std::uint32_t maximum_frontier      = 0;
    std::uint32_t maximum_target_tokens = 1;
    for (std::size_t row = 0; row < lanes.size(); ++row) {
        const std::uint32_t lane = lanes[row];
        if (lane >= max_concurrency ||
            std::find(lanes.begin(), lanes.begin() + static_cast<std::ptrdiff_t>(row), lane) !=
                lanes.begin() + static_cast<std::ptrdiff_t>(row)) {
            throw std::invalid_argument("DFlash batch contains an invalid or duplicate lane");
        }
        const SequenceState& sequence = active_sequence(lane);
        const RequestControl& request = requests[lane];
        if (request.lifecycle != Lifecycle::Active ||
            budgets[row].generated_tokens_remaining == 0 || !sequence.kv ||
            text_kv_addresses->bound_row(sequence.kv->text) < 0 ||
            (backend_kv_cache() && (!sequence.kv->backend ||
                                    backend_kv_addresses->bound_row(*sequence.kv->backend) < 0)) ||
            sequence.execution_frontier >= capacity ||
            sequence.text_kv_valid != sequence.execution_frontier ||
            sequence.dflash_context_frontier > sequence.execution_frontier ||
            sequence.execution_frontier - sequence.dflash_context_frontier > width ||
            sequence.ledger_frontier != sequence.execution_frontier + 1 ||
            sequence.ledger.size() != sequence.ledger_frontier ||
            sequence.prefix_identity.size() != sequence.ledger_frontier ||
            sequence.prefix_digests.size() != sequence.ledger_frontier) {
            throw std::logic_error("DFlash batch row is not decode-ready");
        }
        const std::uint32_t max_by_budget = budgets[row].generated_tokens_remaining > 1
                                                ? budgets[row].generated_tokens_remaining - 1U
                                                : 0U;
        const std::uint32_t extent =
            std::min({draft_window, max_by_budget, capacity - sequence.execution_frontier - 1U});
        maximum_frontier = std::max(maximum_frontier, sequence.execution_frontier);
        maximum_target_tokens =
            std::max(maximum_target_tokens, sequence.execution_frontier + extent + 1U);
    }

    const auto started = Clock::now();
    try {
        std::optional<nvtx::ScopedRange> submit_range;
        submit_range.emplace(nvtx::Name::DecodeDFlashSubmit, nvtx::Category::DFlash,
                             static_cast<std::uint64_t>(lanes.size()));
        DecodeGraphExecutable* executable    = nullptr;
        execution::DFlashEnvelopes envelopes = dflash_envelopes(0, maximum_frontier, draft_window);
        ops::CausalAttentionExecutionEnvelope target_envelope{1, maximum_target_tokens};
        if (use_cuda_graph) {
            DecodeGraphProfile& profile =
                select_graph_profile(dflash_graphs, static_cast<std::uint32_t>(lanes.size()),
                                     maximum_frontier, "DFlash batch");
            executable      = &install_graph_profile(dflash_graphs, profile, "DFlash batch");
            envelopes       = dflash_envelopes(profile.min_execution_frontier,
                                               profile.max_execution_frontier, draft_window);
            target_envelope = {
                1, static_cast<std::uint32_t>(std::min<std::uint64_t>(
                       capacity, static_cast<std::uint64_t>(profile.max_execution_frontier) +
                                     draft_window + 1ULL))};
        }

        for (std::size_t row = 0; row < lanes.size(); ++row) {
            SequenceState& sequence           = active_sequence(lanes[row]);
            const RequestControl& request     = requests[lanes[row]];
            const std::uint32_t frontier      = sequence.execution_frontier;
            const std::uint32_t max_by_budget = budgets[row].generated_tokens_remaining > 1
                                                    ? budgets[row].generated_tokens_remaining - 1U
                                                    : 0U;
            const std::uint32_t extent =
                std::min({draft_window, max_by_budget, capacity - frontier - 1U});
            dflash_host_ingress->anchors[row] = sequence.ledger.back();
            dflash_host_ingress->execution_frontiers[row] =
                checked_i32(frontier, "DFlash batch frontier");
            dflash_host_ingress->context_frontiers[row] =
                checked_i32(sequence.dflash_context_frontier, "DFlash context frontier");
            dflash_host_ingress->proposal_valid_columns[row] = static_cast<std::int32_t>(width);
            dflash_host_ingress->proposal_extents[row]       = static_cast<std::int32_t>(extent);
            dflash_host_ingress->target_valid_columns[row] = static_cast<std::int32_t>(extent + 1U);
            for (std::uint32_t column = 0; column < width; ++column) {
                const std::uint32_t position = frontier + std::min(column, extent);
                dflash_host_ingress->target_rope_positions[row * width + column] =
                    yarn_scale_position(
                        checked_i32(position, "DFlash target RoPE position") + sequence.rope_delta,
                        rope_scaling_original_context, rope_scaling_factor);
            }
            dflash_host_ingress->text_kv_table_rows[row] =
                text_kv_addresses->bound_row(sequence.kv->text);
            dflash_host_ingress->dflash_kv_table_rows[row] =
                sequence.kv->backend ? backend_kv_addresses->bound_row(*sequence.kv->backend) : 0;
            dflash_host_ingress->active_lanes[row]       = static_cast<std::int32_t>(sequence.lane);
            const StateImageSelectors selectors          = state_selectors(sequence);
            dflash_host_ingress->state_source_slots[row] = selectors.source;
            dflash_host_ingress->state_destination_slots[row] = selectors.destination;
            dflash_host_ingress->sampling[row]                = request.sampling_host;
            ensure_sequence_kv_mapped(sequence, frontier + extent + 1U,
                                      backend_kv_cache() ? frontier : 0U);
        }

        execution::DFlashBatchContext schedule_state{
            {device, parameters, work, state_images->linear(),
             replay_records ? &*replay_records : nullptr, io, prefill_hidden, prefill_chunk,
             proposal_head, rope_scaling_factor, rope_scaling_original_context},
            decoder->text_kv,
            *dflash,
            *io.dflash_decode,
            *dflash_host_ingress,
            *dflash_host_egress,
            state_images->continuation_hidden_store()};

        mark_workspace_usage(workspace_plan.dflash_round);
        execution::dflash_decode_batch(schedule_state, static_cast<std::int32_t>(lanes.size()),
                                       draft_window, envelopes, target_envelope, executable);
        submit_range.reset();
        timing.begin_wait();
        {
            nvtx::ScopedRange wait_range(nvtx::Name::DecodeDFlashWait, nvtx::Category::Control,
                                         static_cast<std::uint64_t>(lanes.size()));
            device.synchronize();
        }
        timing.end_wait();

        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            SequenceState& sequence       = active_sequence(lanes[row]);
            RequestControl& request       = requests[lanes[row]];
            const std::uint32_t base_E    = sequence.execution_frontier;
            const std::uint32_t base_S    = sequence.ledger_frontier;
            const std::int32_t count_i    = dflash_host_egress->licensed_counts[row];
            const std::int32_t accepted_i = dflash_host_egress->accepted_drafts[row];
            const std::uint32_t extent =
                static_cast<std::uint32_t>(dflash_host_ingress->proposal_extents[row]);
            if (count_i <= 0 || count_i > static_cast<std::int32_t>(width) || accepted_i < 0 ||
                accepted_i + 1 != count_i || accepted_i > static_cast<std::int32_t>(extent) ||
                static_cast<std::uint32_t>(count_i) > budgets[row].generated_tokens_remaining ||
                static_cast<std::uint64_t>(base_E) + static_cast<std::uint32_t>(count_i) >
                    capacity) {
                throw std::runtime_error("DFlash batch returned invalid row metadata");
            }
            const std::span<const TokenId> row_tokens(dflash_host_egress->licensed_tokens.data() +
                                                          row * width,
                                                      static_cast<std::size_t>(count_i));
            validate_licensed_tokens(row_tokens);
            if (extent == 0) {
                request.speculative_stats.fallback_steps += 1;
            } else {
                request.speculative_stats.rounds += 1;
                request.speculative_stats.drafted_tokens += extent;
                request.speculative_stats.accepted_tokens += static_cast<std::uint32_t>(accepted_i);
                for (std::int32_t i = 0; i < accepted_i; ++i) {
                    request.speculative_stats.accepted_per_position[static_cast<std::size_t>(i)] +=
                        1;
                }
            }
            sequence.dflash_context_frontier = base_E;
            request.pending                  = PendingCandidate{
                                 .kind          = PendingKind::Speculative,
                                 .base_E        = base_E,
                                 .base_S        = base_S,
                                 .prompt_tokens = 0,
                                 .produced      = static_cast<std::uint32_t>(count_i),
            };
            request.lifecycle = Lifecycle::Pending;
            request.timings.decode_seconds += seconds;
        }
        return runtime::BatchedGeneratedRound{
            .tokens     = std::span<const TokenId>(dflash_host_egress->licensed_tokens.data(),
                                                   lanes.size() * width),
            .row_counts = std::span<const std::int32_t>(dflash_host_egress->licensed_counts.data(),
                                                        lanes.size()),
            .row_stride = width,
            .timing     = timing.finish(),
        };
    } catch (...) {
        timing.begin_wait();
        try {
            nvtx::ScopedRange wait_range(nvtx::Name::DecodeDFlashWait, nvtx::Category::Control,
                                         static_cast<std::uint64_t>(lanes.size()));
            device.synchronize();
        } catch (...) {}
        timing.end_wait();
        clear_execution_failure_lanes(lanes);
        throw;
    }
}

runtime::BatchedGeneratedRound
ProgramImpl::decode_raw(std::span<const std::uint32_t> lanes,
                        std::span<const runtime::RoundBudget> budgets,
                        runtime::ExecutionTiming* failed_timing) {
    // Entry-point trace (NINFER_MAT_DEBUG=1): which decode routine actually serves a round.
    if (std::getenv("NINFER_MAT_DEBUG") != nullptr) {
        std::fprintf(stderr, "[mat-debug] DECODE-RAW lanes=%zu backend=%d\n", lanes.size(),
                     static_cast<int>(speculative_backend));
        std::fflush(stderr);
    }
    if (speculative_backend == SpeculativeBackend::None) {
        return decode_ordinary_batch(lanes, budgets, failed_timing);
    }
    if (speculative_backend == SpeculativeBackend::Mtp) {
        return decode_mtp_batch(lanes, budgets, failed_timing);
    }
    return decode_dflash_batch(lanes, budgets, failed_timing);
}

runtime::ExecutionTiming ProgramImpl::resolve_non_speculative_pending(
    SequenceState& sequence, RequestControl& request, std::uint32_t accepted_tokens, bool terminal,
    std::optional<std::uint32_t> prefix_execution_split_after,
    runtime::ExecutionTiming* failed_timing) {
    runtime::ExecutionTimingRecorder timing(runtime::ExecutionTimingPhase::Post, failed_timing);
    if (request.lifecycle != Lifecycle::Pending) {
        throw std::logic_error("pending resolution requires a pending generated round");
    }
    if ((request.pending.kind != PendingKind::Begin &&
         request.pending.kind != PendingKind::Ordinary) ||
        request.pending.produced != 1 || accepted_tokens != 1) {
        throw std::logic_error("non-speculative pending round must commit its single token");
    }

    const std::uint32_t base_ledger_frontier = request.pending.kind == PendingKind::Begin
                                                   ? request.pending.prompt_tokens
                                                   : request.pending.base_S;
    commit_generated_prefix_identity(
        sequence, base_ledger_frontier,
        std::span<const TokenId>(sequence.ledger).subspan(base_ledger_frontier, accepted_tokens),
        prefix_execution_split_after);

    switch (request.pending.kind) {
    case PendingKind::Begin:
        sequence.execution_frontier = request.pending.prompt_tokens;
        sequence.ledger_frontier    = request.pending.prompt_tokens + 1;
        break;
    case PendingKind::Ordinary:
        advance_rebuild_work(sequence, request.pending.base_E + request.pending.produced,
                             prefill_chunk);
        sequence.execution_frontier = request.pending.base_E + request.pending.produced;
        sequence.ledger_frontier    = request.pending.base_S + request.pending.produced;
        break;
    case PendingKind::Speculative:
    case PendingKind::None:
        throw std::logic_error("non-speculative pending round has an invalid kind");
    }
    if (sequence.ledger_frontier != sequence.execution_frontier + 1 ||
        sequence.ledger.size() != sequence.ledger_frontier ||
        sequence.prefix_identity.size() != sequence.ledger_frontier ||
        sequence.prefix_digests.size() != sequence.ledger_frontier) {
        throw std::logic_error("resolved round did not establish a valid frontier");
    }
    // Begin publishes a sampled token but does not execute it through the target. An exact-hit
    // Fork therefore still names an immutable read source and an unwritten destination here; the
    // first state-mutating decode commit closes it. A suffix prefill already closed its Fork at
    // the committed prefill frontier.
    if (request.pending.kind == PendingKind::Begin && terminal && sequence.state.fork_pending) {
        const StateImageSelectors selectors = state_selectors(sequence);
        timing.resume_submit();
        state_images->copy_slot(selectors.source, selectors.destination, device.stream);
        timing.begin_wait();
        device.synchronize();
        timing.end_wait();
        settle_state_fork(sequence);
    } else if (request.pending.kind == PendingKind::Ordinary) {
        settle_state_fork(sequence);
    }
    trim_sequence_kv(sequence, sequence.text_kv_valid, backend_kv_valid(sequence));
    if (terminal) { sequence.mtp_draft_count = 0; }
    request.lifecycle = terminal ? Lifecycle::Finishable : Lifecycle::Active;
    request.pending   = {};
    return timing.finish();
}


} // namespace ninfer::models::qwen3_5::detail
