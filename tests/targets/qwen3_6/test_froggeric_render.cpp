// Render checks for the froggeric v22 template semantics (ported official-template fixes).

#include "targets/qwen3_6/impl/frontend/chat_template.h"

#include <ninfer/types.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fi = ninfer::targets::qwen3_6::frontend_internal;

namespace {

std::string read_fixture(const char* path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int check(bool ok, const char* label) {
    if (!ok) { std::cerr << "FAIL: " << label << "\n"; }
    return ok ? 0 : 1;
}

fi::ChatMessage chat_message(ninfer::ChatRole role, std::string content) {
    fi::ChatMessage message;
    message.role = role;
    message.parts.push_back(fi::ChatPart::text_part(std::move(content)));
    return message;
}

fi::ChatMessage tool_result(std::string id, std::string content) {
    fi::ChatMessage message = chat_message(ninfer::ChatRole::Tool, std::move(content));
    message.tool_call_id    = std::move(id);
    return message;
}

fi::ChatMessage assistant_with_calls(std::string content, std::vector<fi::ToolCall> calls) {
    fi::ChatMessage message = chat_message(ninfer::ChatRole::Assistant, std::move(content));
    message.tool_calls      = std::move(calls);
    return message;
}

const std::string kToolJson =
    R"({"type":"function","function":{"name":"bash","description":"run","parameters":{"type":"object","properties":{"command":{"type":"string"}}}}})";

} // namespace

int main() {
    const std::string source = read_fixture(
        NINFER_SOURCE_DIR "/tests/fixtures/frontend/froggeric_v22_chat_template.jinja");
    int failures = 0;

    fi::CompiledChatTemplate tpl = fi::CompiledChatTemplate::resolve(source); // must not throw

    // 1. tools system block carries the froggeric instructions
    {
        fi::ChatRenderOptions options;
        options.tool_jsons = {kToolJson};
        const std::string out =
            tpl.render({chat_message(ninfer::ChatRole::User, "hi")}, options).text;
        failures += check(out.find("<think>\nBrief explanation of tool call\n</think>\n<tool_call>") !=
                              std::string::npos,
                          "think-first tool example missing");
        failures += check(out.find("IMMEDIATELY after thinking") != std::string::npos,
                          "stricter IMPORTANT block missing");
        failures += check(out.find("Never end a turn with a statement of intent") != std::string::npos,
                          "no-dangling-intent rule missing");
        failures += check(out.find("Do NOT nest <tool_call> blocks") != std::string::npos,
                          "no-nest rule missing");
        failures += check(out.find("do not tell the user about function calls") == std::string::npos,
                          "official instructions leaked");
    }

    // 2. consecutive tool error warnings
    {
        std::vector<fi::ChatMessage> msgs;
        msgs.push_back(chat_message(ninfer::ChatRole::User, "build it"));
        msgs.push_back(assistant_with_calls(
            "", {fi::ToolCall{.id = "1", .name = "bash", .arguments_json = "{\"command\":\"git clone x\"}"}}));
        msgs.push_back(tool_result("1", "fatal: could not read Username"));
        msgs.push_back(assistant_with_calls(
            "", {fi::ToolCall{.id = "2", .name = "bash", .arguments_json = "{\"command\":\"git clone x\"}"}}));
        msgs.push_back(tool_result("2", "fatal: could not read Username"));
        fi::ChatRenderOptions options;
        options.add_generation_prompt = false;
        const std::string out = tpl.render(msgs, options).text;
        failures += check(out.find("SYSTEM WARNING: The previous tool call returned an error") !=
                              std::string::npos,
                          "first-error warning missing");
        failures += check(out.find("SYSTEM WARNING: 2 consecutive tool errors detected") !=
                              std::string::npos,
                          "second-error warning missing");
    }

    // 3. no empty think blocks on replay (preserve_thinking=true, empty reasoning)
    {
        std::vector<fi::ChatMessage> msgs;
        msgs.push_back(chat_message(ninfer::ChatRole::User, "q"));
        msgs.push_back(chat_message(ninfer::ChatRole::Assistant, "answer one"));
        msgs.push_back(chat_message(ninfer::ChatRole::User, "follow up"));
        msgs.push_back(chat_message(ninfer::ChatRole::Assistant, "answer two"));
        fi::ChatRenderOptions options;
        options.add_generation_prompt = false;
        options.preserve_thinking     = true;
        const std::string out         = tpl.render(msgs, options).text;
        failures += check(out.find("<think>") == std::string::npos,
                          "empty think block emitted on replay");
        failures += check(out.find("<|im_start|>assistant\nanswer one") != std::string::npos,
                          "history body missing");
    }

    // 4. multiple tool calls separated by a blank line
    {
        std::vector<fi::ChatMessage> msgs;
        msgs.push_back(chat_message(ninfer::ChatRole::User, "q"));
        msgs.push_back(assistant_with_calls("thinking aloud",
                                            {fi::ToolCall{.id = "1", .name = "a", .arguments_json = "{}"},
                                             fi::ToolCall{.id = "2", .name = "b", .arguments_json = "{}"}}));
        fi::ChatRenderOptions options;
        options.add_generation_prompt = false;
        const std::string out         = tpl.render(msgs, options).text;
        failures += check(out.find("</tool_call>\n\n<tool_call>") != std::string::npos,
                          "calls not blank-line separated");
    }

    // 5. generation prompt with thinking disabled
    {
        fi::ChatRenderOptions options;
        options.enable_thinking = false;
        const std::string out =
            tpl.render({chat_message(ninfer::ChatRole::User, "hi")}, options).text;
        failures += check(out == "<|im_start|>user\nhi<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n",
                          "thinking-off generation prompt wrong");
    }

    // 6. all-tool-response tail: fallback instead of throw
    {
        std::vector<fi::ChatMessage> msgs;
        msgs.push_back(chat_message(ninfer::ChatRole::User, "<tool_response>\nonly\n</tool_response>"));
        fi::ChatRenderOptions options;
        options.add_generation_prompt = false;
        bool threw = false;
        try {
            (void)tpl.render(msgs, options);
        } catch (const std::invalid_argument&) { threw = true; }
        failures += check(!threw, "all-tool-response messages still throw");
    }

    // 7. string arguments rendered raw instead of throwing
    {
        std::vector<fi::ChatMessage> msgs;
        msgs.push_back(chat_message(ninfer::ChatRole::User, "q"));
        msgs.push_back(assistant_with_calls(
            "", {fi::ToolCall{.id = "1", .name = "bash", .arguments_json = "not json at all"}}));
        fi::ChatRenderOptions options;
        options.add_generation_prompt = false;
        bool threw = false;
        std::string out;
        try {
            out = tpl.render(msgs, options).text;
        } catch (const std::invalid_argument&) { threw = true; }
        failures += check(!threw, "string arguments threw");
        failures += check(out.find("<function=bash>\nnot json at all</function>") != std::string::npos,
                          "raw string arguments not rendered verbatim");
    }

    // 8. thinking OFF: replay keeps the empty think block (token-consistency with the
    //    generation prologue, required for append_frontier prefix reuse)
    {
        std::vector<fi::ChatMessage> msgs;
        msgs.push_back(chat_message(ninfer::ChatRole::User, "q"));
        msgs.push_back(chat_message(ninfer::ChatRole::Assistant, "answer one"));
        fi::ChatRenderOptions options;
        options.add_generation_prompt = false;
        options.enable_thinking       = false;
        options.preserve_thinking     = false;
        const std::string out         = tpl.render(msgs, options).text;
        failures += check(out.find("<|im_start|>assistant\n<think>\n\n</think>\n\nanswer one") !=
                              std::string::npos,
                          "thinking-off replay dropped the empty think block (breaks prefix reuse)");
    }

    if (failures == 0) { std::cout << "ALL FROGGERIC RENDER CHECKS PASSED\n"; }
    return failures == 0 ? 0 : 1;
}
