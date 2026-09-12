#include "runtime/contract/sampling.h"

#include <ninfer/targets/qwen3_6_27b/package.h>
#include <ninfer/targets/qwen3_6_35b_a3b/package.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

bool same_preset(const ninfer::SamplingPreset& actual, const ninfer::SamplingPreset& expected) {
    return actual.temperature == expected.temperature && actual.top_k == expected.top_k &&
           actual.top_p == expected.top_p && actual.min_p == expected.min_p &&
           actual.presence_penalty == expected.presence_penalty &&
           actual.frequency_penalty == expected.frequency_penalty;
}

bool throws_invalid(const auto& operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) { return true; }
    return false;
}

bool throws_runtime(const auto& operation) {
    try {
        operation();
    } catch (const std::runtime_error&) { return true; }
    return false;
}

} // namespace

int main() {
    using Dense27 = ninfer::targets::qwen3_6_27b::Package;
    using Moe35   = ninfer::targets::qwen3_6_35b_a3b::Package;

    int failures = 0;

    const ninfer::ModelSamplingDefaults qwen3_6 = Dense27::sampling_defaults(Dense27::model_id);
    const ninfer::ModelSamplingDefaults qwen3_8 =
        Dense27::sampling_defaults(Dense27::qwen3_8_model_id);
    const ninfer::ModelSamplingDefaults qwen3_6_35 = Moe35::sampling_defaults(Moe35::model_id);

    const ninfer::SamplingPreset dense_thinking{
        .temperature = 1.0F, .top_k = 20, .top_p = 0.95F, .min_p = 0.0F};
    const ninfer::SamplingPreset dense_non_thinking{
        .temperature      = 0.7F,
        .top_k            = 20,
        .top_p            = 0.8F,
        .min_p            = 0.0F,
        .presence_penalty = 1.5F,
    };
    const ninfer::SamplingPreset moe_thinking{
        .temperature      = 1.0F,
        .top_k            = 20,
        .top_p            = 0.95F,
        .min_p            = 0.0F,
        .presence_penalty = 1.5F,
    };

    failures += check(same_preset(qwen3_6.thinking, dense_thinking),
                      "Qwen3.6-27B thinking defaults mismatch");
    failures += check(same_preset(qwen3_6.non_thinking, dense_non_thinking),
                      "Qwen3.6-27B non-thinking defaults mismatch");
    failures += check(same_preset(qwen3_8.thinking, dense_thinking) &&
                          same_preset(qwen3_8.non_thinking, dense_non_thinking),
                      "Qwen3.8-27B defaults mismatch");
    const ninfer::SamplingPreset dense_post_thinking{
        .temperature = 0.2F, .top_k = 20, .top_p = 0.95F, .min_p = 0.0F};
    const ninfer::SamplingPreset moe_post_thinking{
        .temperature      = 0.2F,
        .top_k            = 20,
        .top_p            = 0.95F,
        .min_p            = 0.0F,
        .presence_penalty = 1.5F,
    };
    failures += check(same_preset(qwen3_6.post_thinking, dense_post_thinking) &&
                          qwen3_6.has_post_thinking,
                      "Qwen3.6-27B post-thinking defaults mismatch");
    failures += check(same_preset(qwen3_8.post_thinking, dense_post_thinking) &&
                          qwen3_8.has_post_thinking,
                      "Qwen3.8-27B post-thinking defaults mismatch");
    failures += check(same_preset(qwen3_6_35.post_thinking, moe_post_thinking) &&
                          qwen3_6_35.has_post_thinking,
                      "Qwen3.6-35B-A3B post-thinking defaults mismatch");
    failures += check(same_preset(qwen3_6_35.thinking, moe_thinking) &&
                          same_preset(qwen3_6_35.non_thinking, dense_non_thinking),
                      "Qwen3.6-35B-A3B defaults mismatch");
    failures += check(throws_runtime([] { (void)Dense27::sampling_defaults("unknown"); }),
                      "unknown model received dense-27B sampling defaults");

    const ninfer::ResolvedSamplingParameters thinking = ninfer::runtime::resolve_sampling(
        qwen3_8, ninfer::SamplingPhase::Thinking, ninfer::SamplingOverrides{});
    const ninfer::ResolvedSamplingParameters non_thinking = ninfer::runtime::resolve_sampling(
        qwen3_8, ninfer::SamplingPhase::NonThinking, ninfer::SamplingOverrides{});
    failures += check(thinking.temperature == 1.0F && thinking.top_p == 0.95F &&
                          thinking.presence_penalty == 0.0F && thinking.seed == 0,
                      "omitted overrides did not select Qwen3.8 thinking defaults");
    failures += check(non_thinking.temperature == 0.7F && non_thinking.top_p == 0.8F &&
                          non_thinking.presence_penalty == 1.5F,
                      "omitted overrides did not select Qwen3.8 non-thinking defaults");

    const ninfer::ResolvedSamplingParameters post_thinking = ninfer::runtime::resolve_sampling(
        qwen3_8, ninfer::SamplingPhase::PostThinking, ninfer::SamplingOverrides{});
    failures += check(post_thinking.temperature == 0.2F && post_thinking.top_p == 0.95F &&
                          post_thinking.presence_penalty == 0.0F && post_thinking.seed == 0,
                      "omitted overrides did not select Qwen3.8 post-thinking defaults");
    failures += check(
        ninfer::initial_sampling_phase(ninfer::SamplingMode::Thinking) ==
                ninfer::SamplingPhase::Thinking &&
            ninfer::initial_sampling_phase(ninfer::SamplingMode::NonThinking) ==
                ninfer::SamplingPhase::NonThinking,
        "initial sampling phase did not follow the thinking mode");
    const ninfer::SamplingOverrides post_overrides{.temperature = 0.5F};
    const ninfer::ResolvedSamplingParameters post_overridden =
        ninfer::runtime::resolve_sampling(qwen3_8, ninfer::SamplingPhase::PostThinking,
                                         post_overrides);
    failures += check(post_overridden.temperature == 0.5F && post_overridden.top_p == 0.95F,
                      "post-thinking override did not mix with the phase preset");
    const ninfer::ModelSamplingDefaults unregistered{};
    failures += check(!unregistered.has_post_thinking,
                      "unregistered model advertised a post-thinking preset");

    ninfer::SamplingOverrides overrides;
    overrides.temperature       = 0.0F;
    overrides.top_k             = 0;
    overrides.top_p             = 0.0F;
    overrides.min_p             = 0.0F;
    overrides.presence_penalty  = 0.0F;
    overrides.frequency_penalty = -1.0F;
    overrides.seed              = 123;
    const ninfer::ResolvedSamplingParameters overridden =
        ninfer::runtime::resolve_sampling(qwen3_8, ninfer::SamplingPhase::NonThinking, overrides);
    failures += check(overridden.temperature == 0.0F && overridden.top_k == 20 &&
                          overridden.top_p == 0.0F && overridden.presence_penalty == 0.0F &&
                          overridden.frequency_penalty == -1.0F && overridden.seed == 123,
                      "explicit zero sampling overrides were not normalized to the target cap");

    overrides.top_k = 21;
    failures += check(throws_invalid([&] {
                          (void)ninfer::runtime::resolve_sampling(
                              qwen3_8, ninfer::SamplingPhase::Thinking, overrides);
                      }),
                      "top_k beyond the executable candidate domain was accepted");
    overrides.top_k = 0;

    overrides.temperature = std::numeric_limits<float>::quiet_NaN();
    failures += check(throws_invalid([&] {
                          (void)ninfer::runtime::resolve_sampling(
                              qwen3_8, ninfer::SamplingPhase::Thinking, overrides);
                      }),
                      "non-finite sampling override was accepted");

    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
