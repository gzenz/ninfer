#include "options.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

ninfer::cli::Options parse(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) { argv.push_back(argument.data()); }
    return ninfer::cli::parse_options(static_cast<int>(argv.size()), argv.data());
}

bool rejects(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) { return true; }
    return false;
}

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    int failures = 0;
    const ninfer::cli::Options configured =
        parse({"ninfer-cli", "model.ninfer", "--prompt", "hello", "--thinking-budget", "37"});
    failures += check(configured.thinking_budget == 37,
                      "--thinking-budget did not preserve its positive value");
    failures +=
        check(ninfer::cli::usage_text("ninfer-cli").find("--thinking-budget") != std::string::npos,
              "CLI help omits --thinking-budget");
    failures += check(rejects([] {
                          (void)parse({"ninfer-cli", "model.ninfer", "--prompt", "hello",
                                       "--thinking-budget", "0"});
                      }),
                      "zero --thinking-budget was accepted");
    failures += check(rejects([] {
                          (void)parse({"ninfer-cli", "model.ninfer", "--prompt", "hello",
                                       "--thinking-budget", "8", "--no-thinking"});
                      }),
                      "--thinking-budget was accepted with --no-thinking");
    const ninfer::cli::Options with_effort =
        parse({"ninfer-cli", "model.ninfer", "--prompt", "hello", "--thinking-budget", "8",
               "--reasoning-effort", "medium"});
    failures += check(with_effort.thinking_budget == 8 && with_effort.reasoning_effort,
                      "thinking budget did not coexist with reasoning effort");

    const ninfer::cli::Options post_thinking =
        parse({"ninfer-cli", "model.ninfer", "--prompt", "hello",
               "--post-thinking-temperature", "0.2", "--post-thinking-top-p", "0.9",
               "--post-thinking-top-k", "5"});
    failures += check(post_thinking.post_thinking_sampling.temperature &&
                          *post_thinking.post_thinking_sampling.temperature == 0.2F &&
                          *post_thinking.post_thinking_sampling.top_p == 0.9F &&
                          post_thinking.post_thinking_sampling.top_k &&
                          *post_thinking.post_thinking_sampling.top_k == 5,
                      "post-thinking flags did not land in the post-thinking overrides");
    failures += check(!post_thinking.sampling.temperature,
                      "post-thinking flags leaked into the main sampling overrides");
    const ninfer::cli::Options post_thinking_combined = parse(
        {"ninfer-cli", "model.ninfer", "--prompt", "hello",
         "--post-thinking-sampler", "temp=0.25,top_p=0.9,top_k=7,min_p=0.1,presence=0.5,frequency=0.25"});
    failures += check(post_thinking_combined.post_thinking_sampling.temperature &&
                          *post_thinking_combined.post_thinking_sampling.temperature == 0.25F &&
                          *post_thinking_combined.post_thinking_sampling.top_p == 0.9F &&
                          *post_thinking_combined.post_thinking_sampling.top_k == 7 &&
                          *post_thinking_combined.post_thinking_sampling.min_p == 0.1F &&
                          *post_thinking_combined.post_thinking_sampling.presence_penalty ==
                              0.5F &&
                          *post_thinking_combined.post_thinking_sampling.frequency_penalty ==
                              0.25F,
                      "combined --post-thinking-sampler did not populate every field");
    failures += check(rejects([] {
                          (void)parse({"ninfer-cli", "model.ninfer", "--prompt", "hello",
                                       "--post-thinking-top-k", "21"});
                      }),
                      "out-of-range --post-thinking-top-k was accepted");
    failures += check(
        rejects([] {
            (void)parse({"ninfer-cli", "model.ninfer", "--prompt", "hello",
                         "--post-thinking-sampler", "bogus"});
        }),
        "malformed --post-thinking-sampler field was accepted");
    failures += check(
        rejects([] {
            (void)parse({"ninfer-cli", "model.ninfer", "--prompt", "hello",
                         "--post-thinking-sampler", "temp=abc"});
        }),
        "non-numeric --post-thinking-sampler value was accepted");
    const ninfer::cli::Options greedy =
        parse({"ninfer-cli", "model.ninfer", "--prompt", "hello", "--greedy"});
    failures += check(*greedy.sampling.temperature == 0.0F &&
                          *greedy.post_thinking_sampling.temperature == 0.0F,
                      "--greedy did not force both sampling phases to exact argmax");
    failures +=
        check(rejects([] {
                  (void)parse({"ninfer-cli", "model.ninfer", "--prompt", "hello", "--top-k", "21"});
              }),
              "CLI accepted top_k beyond the executable candidate domain");
    return failures == 0 ? 0 : 1;
}
