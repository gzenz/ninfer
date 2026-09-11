// Validates the frontend chat renderer: the context builder maps ChatMessage input onto
// the template variables, the vendored engine executes the registered template, and the
// rendered text matches the Python jinja2 oracle for every fixture context.
//
// The oracle is the Python render of the frontend-shaped context (frontend_oracle/), which
// is the same logical input the C++ context builder produces; the raw-oracle suite covers
// the string-argument shape directly. Contexts that configure template knobs the product
// does not expose yet (tool_call_format, auto_disable_thinking_with_tools, max_tool_*_chars)
// are skipped here.

#include "targets/qwen3_6/impl/frontend/chat_template.h"
#include "targets/qwen3_6/impl/frontend/jinja_chat_render.h"

#include "jinja/jinja.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <optional>
#include <vector>

namespace fi = ninfer::targets::qwen3_6::frontend_internal;
using json     = jinja::json;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

int check(bool ok, const std::string& label) {
    if (!ok) { std::cerr << "FAIL: " << label << '\n'; }
    return ok ? 0 : 1;
}

bool is_media_item(const json& item, const std::string& kind) {
    if (!item.is_object()) { return false; }
    if (item.contains("type") && item["type"].is_string() &&
        item["type"].get<std::string>() == kind) {
        return true;
    }
    if (item.contains(kind)) { return true; }
    return item.contains(kind + "_url");
}

fi::ChatMessage convert_message(const json& source) {
    fi::ChatMessage message;
    const std::string role = source.at("role").get<std::string>();
    if (role == "system") {
        message.role = ninfer::ChatRole::System;
    } else if (role == "developer") {
        message.role = ninfer::ChatRole::Developer;
    } else if (role == "user") {
        message.role = ninfer::ChatRole::User;
    } else if (role == "assistant") {
        message.role = ninfer::ChatRole::Assistant;
    } else if (role == "tool") {
        message.role = ninfer::ChatRole::Tool;
    } else {
        throw std::invalid_argument("unexpected fixture role: " + role);
    }
    if (source.contains("reasoning_content") && source["reasoning_content"].is_string()) {
        message.reasoning_content = source["reasoning_content"].get<std::string>();
    }
    if (source.contains("content") && !source["content"].is_null()) {
        const json& content = source["content"];
        if (content.is_string()) {
            message.parts.push_back(fi::ChatPart::text_part(content.get<std::string>()));
        } else if (content.is_array()) {
            for (const json& item : content) {
                if (item.is_string()) {
                    message.parts.push_back(fi::ChatPart::text_part(item.get<std::string>()));
                } else if (is_media_item(item, "image")) {
                    fi::ChatPart part;
                    part.kind = fi::ChatPartKind::Image;
                    message.parts.push_back(std::move(part));
                } else if (is_media_item(item, "video")) {
                    fi::ChatPart part;
                    part.kind = fi::ChatPartKind::Video;
                    message.parts.push_back(std::move(part));
                } else {
                    message.parts.push_back(
                        fi::ChatPart::text_part(item.value("text", std::string{})));
                }
            }
        }
    }
    if (source.contains("tool_calls") && source["tool_calls"].is_array()) {
        for (const json& call : source["tool_calls"]) {
            fi::ToolCall converted;
            converted.id = call.value("id", std::string{});
            const json& function = call.at("function");
            converted.name = function.at("name").get<std::string>();
            const json& arguments = function.at("arguments");
            converted.arguments_json =
                arguments.is_string() ? arguments.get<std::string>() : arguments.dump();
            message.tool_calls.push_back(std::move(converted));
        }
    }
    return message;
}

bool skip_context(const json& context, std::string& reason) {
    if (context.contains("tool_call_format") &&
        context["tool_call_format"].get<std::string>() != "xml") {
        reason = "tool_call_format";
        return true;
    }
    if (context.value("auto_disable_thinking_with_tools", false)) {
        reason = "auto_disable_thinking_with_tools";
        return true;
    }
    for (const char* key : {"max_tool_arg_chars", "max_tool_response_chars"}) {
        if (context.contains(key) && context[key].get<std::int64_t>() > 0) {
            reason = key;
            return true;
        }
    }
    return false;
}

fi::ChatRenderOptions convert_options(const json& context) {
    fi::ChatRenderOptions options;
    options.enable_thinking = context.value("enable_thinking", true);
    // The fixture contexts omit the key, and the template then adds no generation
    // prompt; ChatRenderOptions defaults to true, so mirror the template here.
    options.add_generation_prompt = context.value("add_generation_prompt", false);
    options.add_vision_id = context.value("add_vision_id", false);
    if (context.contains("preserve_thinking")) {
        options.preserve_thinking = context["preserve_thinking"].get<bool>();
    }
    if (context.contains("reasoning_effort")) {
        const std::string effort = context["reasoning_effort"].get<std::string>();
        if (effort == "low") {
            options.reasoning_effort = ninfer::ReasoningEffort::Low;
        } else if (effort == "medium") {
            options.reasoning_effort = ninfer::ReasoningEffort::Medium;
        } else {
            options.reasoning_effort = ninfer::ReasoningEffort::XHigh;
        }
    }
    if (context.contains("tools")) {
        for (const json& tool : context["tools"]) { options.tool_jsons.push_back(tool.dump()); }
    }
    return options;
}


// Invariants that must hold for every render regardless of the template's content.
int check_invariants(const std::string& name, const fi::RenderedChat& rendered) {
    int failures = 0;
    const std::size_t size = rendered.text.size();
    std::size_t previous = 0;
    bool first = true;
    for (const std::optional<std::size_t> boundary : rendered.message_boundaries) {
        if (!boundary) { continue; }
        if (*boundary > size) {
            failures += fail(name + ": message boundary beyond the rendered text");
            break;
        }
        if (!first && *boundary < previous) {
            failures += fail(name + ": message boundaries are not monotone");
            break;
        }
        previous = *boundary;
        first    = false;
    }
    std::size_t literal_end = 0;
    for (const fi::ByteSpan span : rendered.literal_spans) {
        if (span.begin >= span.end || span.end > size) {
            failures += fail(name + ": literal span outside the rendered text");
            break;
        }
        if (span.begin < literal_end) {
            failures += fail(name + ": literal spans overlap or are unsorted");
            break;
        }
        literal_end = span.end;
    }
    for (std::size_t index = 0; index < rendered.media_placeholders.size(); ++index) {
        const fi::MediaPlaceholderByteSpec& placeholder = rendered.media_placeholders[index];
        if (placeholder.bytes.end > size) {
            failures += fail(name + ": media placeholder outside the rendered text");
            break;
        }
        if (placeholder.item_index != index) {
            failures += fail(name + ": media placeholder item_index is not the render order");
            break;
        }
        const std::string token = rendered.text.substr(placeholder.bytes.begin,
                                                       placeholder.bytes.end - placeholder.bytes.begin);
        const std::string expected_token = placeholder.modality == fi::Modality::Image
                                               ? "<|image_pad|>" : "<|video_pad|>";
        if (token != expected_token) {
            failures += fail(name + ": media placeholder does not cover " + expected_token);
            break;
        }
    }
    for (const std::optional<std::size_t> boundary : rendered.cache_boundaries) {
        if (boundary && *boundary > size) {
            failures += fail(name + ": cache boundary beyond the rendered text");
            break;
        }
    }
    for (const std::size_t boundary : rendered.rewrite_execution_boundaries) {
        if (boundary > size) {
            failures += fail(name + ": rewrite execution boundary beyond the rendered text");
            break;
        }
    }
    if (rendered.rewrite_checkpoint && rendered.rewrite_checkpoint->offset > size) {
        failures += fail(name + ": rewrite checkpoint beyond the rendered text");
    }
    return failures;
}

} // namespace

int main() {
    const std::string fixture_dir = NINFER_SOURCE_DIR "/tests/fixtures/jinja";

    // Every registered template identity: the file is executed by the vendored engine and
    // compared byte-for-byte against the Python oracle for that same file.
    const struct {
        const char* file;
        const char* semantics;
        const char* oracle_dir;
    } identities[] = {
        {"froggeric_v225_chat_template.jinja", "froggeric", "frontend_oracle"},
        {"thinking_toggle_chat_template.jinja", "thinking-toggle",
         "frontend_oracle_thinking_toggle"},
        {"reasoning_effort_chat_template.jinja", "reasoning-effort",
         "frontend_oracle_reasoning_effort"},
    };

    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(fixture_dir + "/contexts")) {
        if (entry.path().extension() != ".json") { continue; }
        names.push_back(entry.path().stem().string());
    }
    std::sort(names.begin(), names.end());

    int failures = 0;
    int checked  = 0;
    int skipped  = 0;

    // The digest gate is what resolves the template embedded in an artifact, so the fixture
    // that an artifact actually carries must stay registered: editing it without updating the
    // registry makes the artifact fail at load time.
    //
    // Only the froggeric fixture is such a file (it is the qwen3.8 artifact's template). The
    // thinking-toggle and reasoning-effort fixtures are stand-ins for the official templates
    // that those artifacts embed; they are reached through an explicit semantics choice, and
    // registering them here would let a file no artifact carries pass the gate. Validating the
    // official files themselves needs the artifact and is tracked in plan.md.
    {
        const std::string registered_source =
            read_file(NINFER_SOURCE_DIR "/tests/fixtures/frontend/froggeric_v225_chat_template.jinja");
        try {
            const fi::CompiledChatTemplate registered =
                fi::CompiledChatTemplate::resolve(registered_source);
            if (!registered.capabilities().reasoning_effort.xhigh) {
                failures += fail("the froggeric fixture lost its effort capabilities");
            }
        } catch (const std::exception& error) {
            failures += fail(std::string("the froggeric fixture is not accepted by the digest "
                                         "gate: ") + error.what());
        }
        for (const char* stand_in : {"thinking_toggle_chat_template.jinja",
                                     "reasoning_effort_chat_template.jinja"}) {
            bool accepted = true;
            try {
                const fi::CompiledChatTemplate unknown = fi::CompiledChatTemplate::resolve(
                    read_file(NINFER_SOURCE_DIR "/tests/fixtures/frontend/" +
                              std::string(stand_in)));
                static_cast<void>(unknown);
            } catch (const std::invalid_argument&) {
                accepted = false;
            }
            failures += check(!accepted, std::string("test stand-in ") + stand_in +
                                             " was registered as an artifact template");
        }
        bool rejected = false;
        try {
            const fi::CompiledChatTemplate unknown =
                fi::CompiledChatTemplate::resolve("{{ 'not a registered template' }}");
            static_cast<void>(unknown);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        failures += check(rejected, "an unregistered template was accepted");
    }

    for (const auto& identity : identities) {
        const std::string template_source =
            read_file(NINFER_SOURCE_DIR "/tests/fixtures/frontend/" + std::string(identity.file));
        const fi::JinjaChatTemplate compiled = fi::JinjaChatTemplate::compile(template_source);
        const std::string oracle_dir = fixture_dir + "/" + identity.oracle_dir;

        for (const std::string& name : names) {
            const json context =
                json::parse(read_file(fixture_dir + "/contexts/" + name + ".json"));
            std::string reason;
            if (skip_context(context, reason)) {
                ++skipped;
                continue;
            }
            std::vector<fi::ChatMessage> messages;
            for (const json& source : context.at("messages")) {
                messages.push_back(convert_message(source));
            }
            const fi::ChatRenderOptions options = convert_options(context);
            const std::string label = std::string(identity.semantics) + "/" + name;
            const std::string error_path = oracle_dir + "/" + name + ".error.expected";
            if (std::filesystem::exists(error_path)) {
                try {
                    const fi::RenderedChat rendered = compiled.render(messages, options);
                    failures += fail(label + " rendered but the oracle raised");
                } catch (const std::exception&) { ++checked; }
                continue;
            }
            const std::string expected = read_file(oracle_dir + "/" + name + ".expected");
            fi::RenderedChat rendered;
            try {
                rendered = compiled.render(messages, options);
            } catch (const std::exception& error) {
                failures += fail(label + " threw: " + error.what());
                continue;
            }
            const std::string& actual = rendered.text;
            if (actual != expected) {
                std::size_t at = 0;
                while (at < std::min(actual.size(), expected.size()) && actual[at] == expected[at]) {
                    ++at;
                }
                failures += fail(label + " differs from the oracle at byte " + std::to_string(at) +
                                 " (" + std::to_string(actual.size()) + " vs " +
                                 std::to_string(expected.size()) + " bytes)");
                continue;
            }
            ++checked;
            failures += check_invariants(label, rendered);
        }
    }
    if (failures == 0) {
        std::cout << "ok (" << checked << " renders, " << skipped << " skipped)\n";
    }
    return failures == 0 ? 0 : 1;
}
