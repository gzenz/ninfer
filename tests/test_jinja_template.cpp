
// Byte-exact parity of the vendored jinja engine against the Python jinja2
// oracle for the registered froggeric v22 chat template. Contexts and
// expected outputs live under tests/fixtures/jinja (oracle: jinja2 3.1.6,
// keep_trailing_newline=True, raise_exception registered as a raising
// callable). This is the oracle the frontend cutover depends on (plan.md,
// "Frontend Template Execution"): the template file, not a C++
// reimplementation, is the source of truth for prompt rendering.

#include "jinja/jinja.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    const std::string template_path =
        NINFER_SOURCE_DIR "/tests/fixtures/frontend/froggeric_v225_chat_template.jinja";
    const std::string context_dir = NINFER_SOURCE_DIR "/tests/fixtures/jinja/contexts";
    const std::string oracle_dir   = NINFER_SOURCE_DIR "/tests/fixtures/jinja/oracle";
    const std::string template_source = read_file(template_path);
    if (template_source.empty()) { return fail("cannot read " + template_path); }
    jinja::Template template_obj(template_source);
    template_obj.add_function("raise_exception", [](const std::vector<jinja::Argument>& args) -> jinja::json {
        throw std::runtime_error(args.empty() ? "raise_exception"
                                              : args[0].second.get<std::string>());
    });

    int failures = 0;
    std::size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(context_dir)) {
        if (entry.path().extension() != ".json") { continue; }
        const std::string stem = entry.path().stem().string();
        const jinja::json context = jinja::json::parse(read_file(entry.path().string()));
        const bool expect_error =
            std::filesystem::exists(oracle_dir + "/" + stem + ".error.expected");
        if (expect_error) {
            const std::string expected_error = read_file(oracle_dir + "/" + stem + ".error.expected");
            try {
                template_obj.render(context);
                failures += fail(stem + " rendered but the oracle raised: " + expected_error);
            } catch (const std::exception& e) {
                if (std::string(e.what()) != expected_error) {
                    failures += fail(stem + " error mismatch: '" + std::string(e.what()) +
                                    "' != '" + expected_error + "'");
                } else { ++checked; }
            }
            continue;
        }
        std::cerr << stem << " ... " << std::flush;
        const std::string expected = read_file(oracle_dir + "/" + stem + ".expected");
        std::string out;
        try {
            out = template_obj.render(context);
        } catch (const std::exception& error) {
            failures += fail(stem + " raised: " + error.what());
            continue;
        }
        std::cerr << "ok\n" << std::flush;
        if (out != expected) {
            failures += fail(stem + " differs from oracle (" + std::to_string(out.size()) +
                             " vs " + std::to_string(expected.size()) + " bytes)");
        } else { ++checked; }
    }
    if (checked != 33) {
        failures += fail("expected 33 parity contexts, checked " + std::to_string(checked));
    }
    if (failures == 0) { std::cout << "ok (" << checked << " contexts)\n"; }
    return failures;
}
