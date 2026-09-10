
// Byte-exact parity of the vendored jinja engine (third_party/jinja) against
// the Python jinja2 oracle for the registered froggeric chat template. The
// oracle outputs were generated with jinja2 3.1.6 (keep_trailing_newline=True,
// raise_exception registered as a raising callable). The template file itself
// is the source of truth; this test proves the C++ engine executes it
// identically, which is what the frontend cutover depends on.

#include "jinja/jinja.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string first_line_of(const std::string& text) {
    const std::size_t end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}

} // namespace

int main() {
    const std::string template_path =
        NINFER_SOURCE_DIR "/tests/fixtures/frontend/froggeric_v22_chat_template.jinja";
    const std::string context_dir = NINFER_SOURCE_DIR "/tests/fixtures/jinja/contexts";
    const std::string oracle_dir   = NINFER_SOURCE_DIR "/tests/fixtures/jinja/oracle";
    const std::string template_source = read_file(template_path);
    if (template_source.empty()) {
        std::cerr << "FAIL: cannot read " << template_path << "\n";
        return 1;
    }

    jinja::Template template_obj(template_source);
    template_obj.add_function("raise_exception", [](const std::vector<jinja::Argument>& args) -> jinja::json {
        throw std::runtime_error(args.empty() ? "raise_exception" : args[0].second.get<std::string>());
    });

    std::vector<std::string> context_names;
    for (const auto& entry : std::filesystem::directory_iterator(context_dir)) {
        if (entry.path().extension() == ".json") { context_names.push_back(entry.path().filename().string()); }
    }
    std::sort(context_names.begin(), context_names.end());
    if (context_names.size() != 24) {
        std::cerr << "FAIL: expected 24 parity contexts, found " << context_names.size() << "\n";
        return 1;
    }

    int failures = 0;
    for (const std::string& name : context_names) {
        const std::string stem = name.substr(0, name.size() - 5);
        const jinja::json context = jinja::json::parse(read_file(context_dir + "/" + name));
        const std::string error_path = oracle_dir + "/" + stem + ".error.expected";
        const bool expect_error = std::filesystem::exists(error_path);
        if (expect_error) {
            const std::string expected_error = read_file(error_path);
            try {
                template_obj.render(context);
                std::cerr << "FAIL: " << stem << " rendered but the oracle raised: "
                          << expected_error << "\n";
                ++failures;
            } catch (const std::exception& e) {
                if (first_line_of(e.what()) != first_line_of(expected_error)) {
                    std::cerr << "FAIL: " << stem << " error mismatch: '" << e.what()
                              << "' != '" << first_line_of(expected_error) << "'\n";
                    ++failures;
                }
            }
            continue;
        }
        const std::string expected = read_file(oracle_dir + "/" + stem + ".expected");
        try {
            const std::string out = template_obj.render(context);
            if (out != expected) {
                std::cerr << "FAIL: " << stem << " differs from oracle (" << out.size()
                          << " vs " << expected.size() << " bytes)\n";
                ++failures;
            }
        } catch (const std::exception& e) {
            std::cerr << "FAIL: " << stem << " unexpected exception: " << e.what() << "\n";
            ++failures;
        }
    }

    if (failures == 0) {
        std::cout << "ok (" << context_names.size() << " contexts)\n";
        return 0;
    }
    std::cerr << failures << " of " << context_names.size() << " contexts failed\n";
    return 1;
}
