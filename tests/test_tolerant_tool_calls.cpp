// Tolerant-mode recovery checks, including the real drift captured from a
// Claude Code session (opening wrapper present, duplicate/suffix garbage).
#include "serve/tool_call_parser.h"
#include <iostream>
#include <string>

using ninfer::serve::parse_qwen_tool_call_output;

static int check(bool ok, const char* label) {
    if (!ok) std::cerr << "FAIL: " << label << "\n";
    return ok ? 0 : 1;
}

int main() {
    int f = 0;
    const std::size_t kMaxName = 64;

    // 1. well-formed call parses in BOTH modes
    {
        const std::string good =
            "<tool_call>\n<function=bash>\n<parameter=command>\nls\n</parameter>\n</function>\n</tool_call>";
        auto strict = parse_qwen_tool_call_output(good, kMaxName, false);
        auto tol    = parse_qwen_tool_call_output(good, kMaxName, true);
        f += check(strict.is_tool_call_response && strict.tool_calls.size() == 1, "strict: good call");
        f += check(tol.is_tool_call_response && tol.tool_calls.size() == 1, "tolerant: good call");
        f += check(tol.tool_calls[0].name == "bash", "tolerant: name parsed");
    }

    // 2. trailing suffix after a complete call: strict rejects, tolerant recovers
    {
        const std::string drift =
            "<tool_call>\n<function=bash>\n<parameter=command>\nls\n</parameter>\n</function>\n"
            "</function>\n</tool_call>";
        auto strict = parse_qwen_tool_call_output(drift, kMaxName, false);
        auto tol    = parse_qwen_tool_call_output(drift, kMaxName, true);
        f += check(!strict.is_tool_call_response, "strict: rejects duplicate closer");
        f += check(tol.is_tool_call_response && tol.tool_calls.size() == 1,
                   "tolerant: RECOVERS duplicate closer");
    }

    // 3. missing </tool_call> entirely
    {
        const std::string unclosed =
            "<tool_call>\n<function=bash>\n<parameter=command>\nls -la\n</parameter>\n</function>";
        auto strict = parse_qwen_tool_call_output(unclosed, kMaxName, false);
        auto tol    = parse_qwen_tool_call_output(unclosed, kMaxName, true);
        f += check(!strict.is_tool_call_response, "strict: rejects unclosed");
        f += check(tol.is_tool_call_response && tol.tool_calls.size() == 1,
                   "tolerant: RECOVERS unclosed");
    }

    // 4. explanatory prose after a complete call
    {
        const std::string chatty =
            "<tool_call>\n<function=bash>\n<parameter=command>\nls\n</parameter>\n</function>\n</tool_call>"
            "\nI will now list the files.";
        auto tol = parse_qwen_tool_call_output(chatty, kMaxName, true);
        f += check(tol.is_tool_call_response && tol.tool_calls.size() == 1,
                   "tolerant: recovers despite trailing prose");
    }

    // 5. genuinely unparseable text must still fall back in tolerant mode
    {
        const std::string junk = "<tool_call>\nthis is not a function at all\n</tool_call>";
        auto tol = parse_qwen_tool_call_output(junk, kMaxName, true);
        f += check(!tol.is_tool_call_response, "tolerant: still falls back on junk");
    }

    // 6. plain prose with no tool call is untouched
    {
        const std::string prose = "Here are the files you asked about.";
        auto tol = parse_qwen_tool_call_output(prose, kMaxName, true);
        f += check(!tol.is_tool_call_response && tol.content == prose, "tolerant: prose untouched");
    }

    if (f == 0) std::cout << "ALL TOLERANT RECOVERY CHECKS PASSED\n";
    return f == 0 ? 0 : 1;
}
