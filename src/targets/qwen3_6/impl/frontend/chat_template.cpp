#include "targets/qwen3_6/impl/frontend/chat_template.h"

#include "targets/qwen3_6/impl/frontend/digest.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ninfer::targets::qwen3_6::frontend_internal {
namespace {

using OrderedJson = nlohmann::ordered_json;

constexpr Sha256Digest kThinkingToggleTemplateDigest{
    0xe8, 0x4f, 0x32, 0xa2, 0x3f, 0xdd, 0xa2, 0x76, 0x89, 0xf8, 0x68, 0xaa, 0x4a, 0x1a, 0x56, 0x21,
    0xf4, 0x11, 0x33, 0xe5, 0x1a, 0x48, 0xd7, 0xf3, 0xef, 0xcb, 0xea, 0x28, 0x39, 0x57, 0x42, 0x59,
};

constexpr Sha256Digest kReasoningEffortTemplateDigest{
    0xc3, 0xcf, 0x9e, 0x34, 0xab, 0xf4, 0xf9, 0xe3, 0x6c, 0x2d, 0x72, 0x16, 0x5a, 0xa9, 0xc1, 0x32,
    0xd3, 0xe2, 0xa7, 0x25, 0xb6, 0xc2, 0x58, 0x6a, 0xaa, 0x3a, 0x8a, 0xf9, 0xd7, 0xa8, 0x10, 0x41,
};

// qwen3.8-froggeric-v22 (Qwen-Fixed-Chat-Templates): stricter tool-call instructions,
// consecutive tool-error warnings, no empty think blocks on replay.
constexpr Sha256Digest kFroggericV22TemplateDigest{
    0x39, 0x8e, 0xdf, 0x5b, 0x5b, 0xb8, 0x02, 0xfb, 0x6b, 0x9c, 0x9a, 0x8d, 0xba, 0x67, 0x0d, 0x09,
    0xf2, 0xaa, 0xee, 0xf6, 0xfd, 0xca, 0xa0, 0xb2, 0xca, 0x30, 0x72, 0x65, 0xf5, 0x9f, 0x78, 0xdc,
};

constexpr std::string_view kLowReasoningInstructions =
    "Reasoning effort is set to low. Keep your thinking brief and focused, moving directly to "
    "the conclusion without unnecessary elaboration.";

constexpr std::string_view kXHighReasoningInstructions =
    "Reasoning effort is set to xhigh. Please think carefully through the task, validate key "
    "assumptions, consider plausible alternatives, and prioritize correctness, consistency, and "
    "clarity in the final answer.";

bool is_instruction_role(ChatRole role) noexcept {
    return role == ChatRole::System || role == ChatRole::Developer;
}

void validate_instruction_message(const ChatMessage& message) {
    if (message.has_media()) {
        throw std::invalid_argument(
            "system and developer messages cannot contain images or videos");
    }
    if (!message.reasoning_content.empty() || !message.tool_calls.empty() ||
        !message.tool_call_id.empty()) {
        throw std::invalid_argument("system and developer messages may contain only text content");
    }
}

std::string trim_ascii_whitespace(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) { --end; }
    return text.substr(begin, end - begin);
}

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

long last_real_user_query(const std::vector<ChatMessage>& messages) {
    for (long i = static_cast<long>(messages.size()) - 1; i >= 0; --i) {
        const ChatMessage& message = messages[static_cast<std::size_t>(i)];
        if (message.role != ChatRole::User) { continue; }
        const std::string content = trim_ascii_whitespace(message.rendered_content());
        if (!(starts_with(content, "<tool_response>") && ends_with(content, "</tool_response>"))) {
            return i;
        }
    }
    throw std::invalid_argument("no user query found in chat messages");
}

std::string lstrip_newlines(std::string text) {
    std::size_t begin = 0;
    while (begin < text.size() && text[begin] == '\n') { ++begin; }
    return text.substr(begin);
}

std::string rstrip_newlines(std::string text) {
    std::size_t end = text.size();
    while (end > 0 && text[end - 1] == '\n') { --end; }
    return text.substr(0, end);
}

// Split an assistant turn into (reasoning, content) exactly as the Qwen3.6 jinja
// does when reasoning_content is not provided: reasoning is the text between the
// last <think> and the first </think>; content is everything after the last
// </think>. When there is no </think> the whole thing is content and reasoning is
// empty.
struct ThinkParts {
    std::string reasoning;
    std::string content;
};

ThinkParts derive_think_parts(const std::string& content) {
    ThinkParts parts;
    const std::size_t first_close = content.find("</think>");
    if (first_close == std::string::npos) {
        parts.content = content;
        return parts;
    }
    // reasoning = content.split('</think>')[0].rstrip('\n').split('<think>')[-1].lstrip('\n')
    std::string before          = rstrip_newlines(content.substr(0, first_close));
    const std::size_t last_open = before.rfind("<think>");
    std::string reasoning       = (last_open == std::string::npos)
                                      ? before
                                      : before.substr(last_open + std::string("<think>").size());
    parts.reasoning             = lstrip_newlines(std::move(reasoning));
    // content = content.split('</think>')[-1].lstrip('\n')
    const std::size_t last_close = content.rfind("</think>");
    parts.content = lstrip_newlines(content.substr(last_close + std::string("</think>").size()));
    return parts;
}

constexpr std::string_view kToolInstructions =
    "\n\nIf you choose to call a function ONLY reply in the following format with NO suffix:\n\n"
    "<tool_call>\n"
    "<function=example_function_name>\n"
    "<parameter=example_parameter_1>\n"
    "value_1\n"
    "</parameter>\n"
    "<parameter=example_parameter_2>\n"
    "This is the value for the second parameter\n"
    "that can span\n"
    "multiple lines\n"
    "</parameter>\n"
    "</function>\n"
    "</tool_call>\n\n"
    "<IMPORTANT>\n"
    "Reminder:\n"
    "- Function calls MUST follow the specified format: an inner <function=...></function> block "
    "must be nested within <tool_call></tool_call> XML tags\n"
    "- Required parameters MUST be specified\n"
    "- You may provide optional reasoning for your function call in natural language BEFORE the "
    "function call, but NOT after\n"
    "- If there is no function call available, answer the question like normal with your current "
    "knowledge and do not tell the user about function calls\n"
    "</IMPORTANT>";

// froggeric v22 tool instructions (xml branch): think-first example + stricter format rules.
constexpr std::string_view kFroggericToolInstructions =
    "\n\nIf you choose to call a function ONLY reply in the following format with NO suffix:\n\n"
    "<think>\n"
    "Brief explanation of tool call\n"
    "</think>\n"
    "<tool_call>\n"
    "<function=example_function_name>\n"
    "<parameter=example_parameter_1>\n"
    "value_1\n"
    "</parameter>\n"
    "<parameter=example_parameter_2>\n"
    "This is the value for the second parameter\n"
    "that can span\n"
    "multiple lines\n"
    "</parameter>\n"
    "</function>\n"
    "</tool_call>\n\n"
    "<IMPORTANT>\n"
    "Reminder:\n"
    "- You can use the <think></think> block to plan your next tool call OR to synthesize data and "
    "formulate your final response to the user.\n"
    "- ALL explanation and reasoning MUST be placed strictly inside the <think></think> block.\n"
    "- Function calls MUST follow the specified format: an inner <function=...></function> block "
    "must be nested within <tool_call></tool_call> XML tags\n"
    "- If you choose to call a tool, you MUST output the <tool_call> block IMMEDIATELY after "
    "thinking, with NO conversational text before it.\n"
    "- Never end a turn with a statement of intent (e.g. \"Let me check X\"). Either execute it "
    "now with a tool call, or state explicitly that the task is complete.\n"
    "- The <tool_call> and <function> tags MUST be at the very beginning of a new line, with NO "
    "spaces or indentation before them.\n"
    "- To call multiple functions, output a separate, completely closed <tool_call></tool_call> "
    "block for EACH function. Do NOT nest <tool_call> blocks.\n"
    "- If you have all necessary data, provide your final answer directly to the user without any "
    "tool call.\n"
    "</IMPORTANT>";

// froggeric v22: split assistant content on any of the think-close variants the model
// actually emits (official only handles a clean </think>).
ThinkParts derive_think_parts_froggeric(const std::string& content) {
    static constexpr std::array<std::string_view, 6> kClosers = {
        "</think>", "</thinking>", "\n</think>", "\n</thinking>", "\n</ think>", "\n</think >",
    };
    std::string_view closer;
    if (starts_with(content, "</think>")) {
        closer = kClosers[0];
    } else if (starts_with(content, "</thinking>")) {
        closer = kClosers[1];
    } else {
        for (std::size_t k = 2; k < kClosers.size(); ++k) {
            if (content.find(kClosers[k]) != std::string::npos) { closer = kClosers[k]; break; }
        }
    }
    ThinkParts parts;
    if (closer.empty()) {
        parts.content = content;
        return parts;
    }
    const std::size_t first_close = content.find(closer);
    std::string reasoning       = rstrip_newlines(content.substr(0, first_close));
    const std::string_view opener =
        closer.find("thinking") != std::string_view::npos ? "<thinking>" : "<think>";
    const std::size_t last_open = reasoning.rfind(opener);
    if (last_open != std::string::npos) {
        reasoning = reasoning.substr(last_open + opener.size());
    }
    parts.reasoning        = lstrip_newlines(std::move(reasoning));
    const std::size_t last = content.rfind(closer);
    parts.content = lstrip_newlines(content.substr(last + closer.size()));
    return parts;
}

std::string ascii_lower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') { ch = static_cast<char>(ch - 'A' + 'a'); }
    }
    return text;
}

// froggeric v22: heuristic marking a tool result as a failure for the consecutive-error
// circuit breaker (short error-looking output, not a command echo or timing line).
bool looks_like_tool_error(const std::string& content) {
    if (content.size() >= 500) { return false; }
    if (content.find("$ ") != std::string::npos) { return false; }
    const std::string lower = ascii_lower(content);
    if (lower.find("took ") != std::string::npos) { return false; }
    const std::string head = lower.substr(0, 80);
    static constexpr std::array<std::string_view, 9> kMarkers = {
        "\"error\":", "error:", "err!", "fatal:", "exception:",
        "traceback", "command not found", "invalid syntax", "failed to",
    };
    for (const std::string_view marker : kMarkers) {
        if (head.find(marker) != std::string::npos) { return true; }
    }
    return false;
}

constexpr std::string_view kToolErrorWarningSuffix1 =
    "\n\n\xE2\x9A\xA0\xEF\xB8\x8F SYSTEM WARNING: The previous tool call returned an error. "
    "Diagnose the failure and retry with completely corrected arguments.";

std::string tool_error_warning_suffix_n(int failures) {
    return "\n\n\xE2\x9A\xA0\xEF\xB8\x8F SYSTEM WARNING: " + std::to_string(failures) +
           " consecutive tool errors detected. Your previous approach is incorrect. You MUST use "
           "a fundamentally different approach or corrected arguments.";
}

std::string tojson_text(const OrderedJson& value) {
    if (value.is_array()) {
        std::string rendered = "[";
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (index != 0) { rendered += ", "; }
            rendered += tojson_text(value[index]);
        }
        rendered += "]";
        return rendered;
    }
    if (value.is_object()) {
        std::string rendered = "{";
        std::size_t index    = 0;
        for (auto it = value.begin(); it != value.end(); ++it, ++index) {
            if (index != 0) { rendered += ", "; }
            rendered += OrderedJson(it.key()).dump();
            rendered += ": ";
            rendered += tojson_text(it.value());
        }
        rendered += "}";
        return rendered;
    }
    return value.dump();
}

std::string parameter_text(const OrderedJson& value) {
    if (value.is_string()) { return value.get<std::string>(); }
    return tojson_text(value);
}

std::string render_tool_call(const ToolCall& call, bool allow_empty_arguments,
                             bool raw_string_fallback = false) {
    if (allow_empty_arguments && call.arguments_json.empty()) {
        return "<tool_call>\n<function=" + call.name + ">\n</function>\n</tool_call>";
    }
    OrderedJson args;
    if (raw_string_fallback &&
        (!OrderedJson::accept(call.arguments_json) ||
         !(args = OrderedJson::parse(call.arguments_json)).is_object())) {
        // froggeric v22: string arguments render verbatim after the function tag
        return "<tool_call>\n<function=" + call.name + ">\n" + call.arguments_json +
               "</function>\n</tool_call>";
    }
    if (!raw_string_fallback) { args = OrderedJson::parse(call.arguments_json); }
    if (!args.is_object()) {
        throw std::invalid_argument("tool call arguments must be a JSON object");
    }

    std::string rendered;
    rendered += "<tool_call>\n<function=";
    rendered += call.name;
    rendered += ">\n";
    for (auto it = args.begin(); it != args.end(); ++it) {
        rendered += "<parameter=";
        rendered += it.key();
        rendered += ">\n";
        rendered += parameter_text(it.value());
        rendered += "\n</parameter>\n";
    }
    rendered += "</function>\n</tool_call>";
    return rendered;
}

std::string render_tools_system_block(const std::vector<std::string>& tool_jsons,
                                      const std::string& leading_instruction,
                                      std::string_view reasoning_instructions,
                                      bool froggeric) {
    std::string rendered;
    rendered += "<|im_start|>system\n";
    if (!reasoning_instructions.empty()) {
        rendered += reasoning_instructions;
        rendered += "\n\n";
    }
    rendered += "# Tools\n\nYou have access to the following functions:\n\n<tools>";
    for (const std::string& tool : tool_jsons) {
        rendered += "\n";
        rendered += tojson_text(OrderedJson::parse(tool));
    }
    rendered += "\n</tools>";
    rendered += std::string(froggeric ? kFroggericToolInstructions : kToolInstructions);
    if (!leading_instruction.empty()) {
        rendered += "\n\n";
        rendered += leading_instruction;
    }
    rendered += "<|im_end|>\n";
    return rendered;
}

std::string_view resolve_reasoning_instructions(ChatTemplateSemantics semantics,
                                                const ChatRenderOptions& options) {
    if (semantics == ChatTemplateSemantics::ThinkingToggle) {
        if (options.reasoning_effort) {
            throw std::invalid_argument("loaded chat template does not support reasoning effort");
        }
        return {};
    }
    if (!options.enable_thinking) {
        if (options.reasoning_effort) {
            throw std::invalid_argument(
                "reasoning effort cannot be combined with disabled thinking");
        }
        return {};
    }

    switch (options.reasoning_effort.value_or(ReasoningEffort::XHigh)) {
    case ReasoningEffort::Low:
        return kLowReasoningInstructions;
    case ReasoningEffort::Medium:
        return {};
    case ReasoningEffort::XHigh:
        return kXHighReasoningInstructions;
    }
    throw std::invalid_argument("invalid reasoning effort");
}

} // namespace

bool ChatMessage::has_media() const noexcept {
    for (const ChatPart& part : parts) {
        if (part.kind != ChatPartKind::Text) { return true; }
    }
    return false;
}

std::string ChatMessage::rendered_content(bool add_vision_id, int* image_count,
                                          int* video_count) const {
    int local_images = 0;
    int local_videos = 0;
    int& images      = image_count == nullptr ? local_images : *image_count;
    int& videos      = video_count == nullptr ? local_videos : *video_count;
    std::string out;
    for (const ChatPart& part : parts) {
        switch (part.kind) {
        case ChatPartKind::Text:
            out += part.text;
            break;
        case ChatPartKind::Image:
            ++images;
            if (add_vision_id) { out += "Picture " + std::to_string(images) + ": "; }
            out += "<|vision_start|><|image_pad|><|vision_end|>";
            break;
        case ChatPartKind::Video:
            ++videos;
            if (add_vision_id) { out += "Video " + std::to_string(videos) + ": "; }
            out += "<|vision_start|><|video_pad|><|vision_end|>";
            break;
        }
    }
    return out;
}

CompiledChatTemplate CompiledChatTemplate::resolve(std::string_view source) {
    const Sha256Digest digest = sha256(source);
    if (digest == kThinkingToggleTemplateDigest) {
        return CompiledChatTemplate(ChatTemplateSemantics::ThinkingToggle);
    }
    if (digest == kReasoningEffortTemplateDigest) {
        return CompiledChatTemplate(ChatTemplateSemantics::ReasoningEffort);
    }
    if (digest == kFroggericV22TemplateDigest) {
        return CompiledChatTemplate(ChatTemplateSemantics::FroggericV22);
    }
    throw std::invalid_argument("unsupported frontend/chat_template.jinja (sha256 " +
                                sha256_hex(digest) + ")");
}

PromptCapabilities CompiledChatTemplate::capabilities() const noexcept {
    PromptCapabilities result;
    result.enable_thinking = true;
    if (semantics_ == ChatTemplateSemantics::ReasoningEffort ||
        semantics_ == ChatTemplateSemantics::FroggericV22) {
        result.reasoning_effort.low            = true;
        result.reasoning_effort.medium         = true;
        result.reasoning_effort.xhigh          = true;
        result.reasoning_effort.default_effort = ReasoningEffort::XHigh;
    }
    return result;
}

RenderedChat CompiledChatTemplate::render(const std::vector<ChatMessage>& messages,
                                          ChatRenderOptions options) const {
    if (messages.empty()) { throw std::invalid_argument("chat messages must not be empty"); }

    const bool effort_template = semantics_ == ChatTemplateSemantics::ReasoningEffort;
    const bool froggeric       = semantics_ == ChatTemplateSemantics::FroggericV22;
    const bool effort_like     = effort_template || froggeric;
    const std::string_view reasoning_instructions =
        resolve_reasoning_instructions(semantics_, options);

    std::size_t message_begin = 0;
    std::string leading_instruction;
    if (is_instruction_role(messages[0].role)) {
        validate_instruction_message(messages[0]);
        leading_instruction = trim_ascii_whitespace(messages[0].rendered_content());
        message_begin       = 1;
    }

    std::string rendered;
    const bool has_tools = !options.tool_jsons.empty();
    if (has_tools) {
        rendered += render_tools_system_block(options.tool_jsons, leading_instruction,
                                              reasoning_instructions, froggeric);
    } else if (message_begin == 1) {
        if (!effort_like || !leading_instruction.empty() || !reasoning_instructions.empty()) {
            rendered += "<|im_start|>system\n";
            if (!reasoning_instructions.empty()) {
                rendered += reasoning_instructions;
                if (!leading_instruction.empty()) { rendered += "\n\n"; }
            }
            rendered += leading_instruction;
            rendered += "<|im_end|>\n";
        }
    } else if (!reasoning_instructions.empty()) {
        rendered += "<|im_start|>system\n";
        rendered += reasoning_instructions;
        rendered += "<|im_end|>\n";
    }

    long last_query_index = 0;
    if (froggeric) {
        // froggeric v22: fall back instead of raising when every user message is a tool response
        try {
            last_query_index = last_real_user_query(messages);
        } catch (const std::invalid_argument&) {
            last_query_index = static_cast<long>(messages.size()) - 1 > 50
                                   ? static_cast<long>(messages.size()) - 1
                                   : 0;
        }
    } else {
        last_query_index = last_real_user_query(messages);
    }
    const bool preserve_thinking = options.preserve_thinking.value_or(effort_like);
    std::optional<RewriteCheckpointByteSpec> rewrite_checkpoint;
    int consecutive_tool_failures = 0;

    int image_count = 0;
    int video_count = 0;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const ChatMessage& message = messages[i];
        if (i < message_begin) { continue; }
        if (is_instruction_role(message.role)) { validate_instruction_message(message); }
        const std::string content = trim_ascii_whitespace(
            message.rendered_content(options.add_vision_id, &image_count, &video_count));
        if (is_instruction_role(message.role)) {
            rendered += "<|im_start|>system\n";
            rendered += content;
            rendered += "<|im_end|>\n";
            continue;
        }
        if (message.role == ChatRole::User) {
            rendered += "<|im_start|>user\n";
            rendered += content;
            rendered += "<|im_end|>\n";
            continue;
        }
        if (message.role == ChatRole::Tool) {
            const bool opens_group = i > 0 && messages[i - 1].role != ChatRole::Tool;
            const bool closes_group =
                i + 1 == messages.size() || messages[i + 1].role != ChatRole::Tool;
            if (froggeric) {
                consecutive_tool_failures =
                    looks_like_tool_error(content) ? consecutive_tool_failures + 1 : 0;
            }
            if (opens_group) { rendered += "<|im_start|>user"; }
            rendered += "\n<tool_response>\n";
            rendered += content;
            if (froggeric && consecutive_tool_failures >= 2) {
                rendered += tool_error_warning_suffix_n(consecutive_tool_failures);
            } else if (froggeric && consecutive_tool_failures == 1) {
                rendered += kToolErrorWarningSuffix1;
            }
            rendered += "\n</tool_response>";
            if (closes_group) { rendered += "<|im_end|>\n"; }
            continue;
        }

        if (message.role != ChatRole::Assistant) {
            throw std::invalid_argument("unsupported chat role value");
        }

        // assistant
        std::string reasoning;
        std::string body = content;
        if (!message.reasoning_content.empty()) {
            reasoning = message.reasoning_content;
        } else if (semantics_ == ChatTemplateSemantics::ThinkingToggle) {
            ThinkParts parts = derive_think_parts(content);
            reasoning        = std::move(parts.reasoning);
            body             = std::move(parts.content);
        } else if (froggeric) {
            ThinkParts parts = derive_think_parts_froggeric(content);
            reasoning        = std::move(parts.reasoning);
            body             = std::move(parts.content);
        }
        reasoning = trim_ascii_whitespace(reasoning);

        // froggeric v22: suppress empty think blocks on replay only when thinking is
        // enabled. With thinking disabled the generation prologue itself is an empty
        // think block, so replay must keep it to stay token-consistent for prefix reuse.
        const bool keep_thinking = (preserve_thinking || (static_cast<long>(i) > last_query_index)) &&
                                   (!froggeric || !reasoning.empty() || !options.enable_thinking);
        rendered += "<|im_start|>assistant\n";
        // Anchor the turn-closure checkpoint at the LAST reasoning-stripped assistant
        // turn rather than the first. Every turn past last_query_index has its
        // reasoning dropped when preserve_thinking is false, so each one is an
        // equally valid restore point - but keeping only the earliest means the
        // checkpoint never advances, and a later history rewrite has to replay
        // everything after it (measured: ttft grew 750ms -> 2.3s over 8 turns, and
        // up to 12s at 28k drift). Taking the latest keeps the anchor near the
        // frontier so the restore path stays cheap.
        if (!preserve_thinking && static_cast<long>(i) > last_query_index) {
            rewrite_checkpoint = RewriteCheckpointByteSpec{
                .kind = RewriteCheckpointKind::TurnClosure, .offset = rendered.size()};
        }
        if (keep_thinking) {
            rendered += "<think>\n";
            rendered += reasoning;
            rendered += "\n</think>\n\n";
        }
        rendered += body;
        if (!message.tool_calls.empty()) {
            const bool body_has_text = !trim_ascii_whitespace(body).empty();
            for (std::size_t call_index = 0; call_index < message.tool_calls.size(); ++call_index) {
                if (call_index == 0) {
                    if (body_has_text) { rendered += "\n\n"; }
                } else {
                    rendered += froggeric ? "\n\n" : "\n";
                }
                rendered += render_tool_call(message.tool_calls[call_index], effort_like,
                                             froggeric);
            }
        }
        rendered += "<|im_end|>\n";
    }

    if (options.add_generation_prompt) {
        rendered += "<|im_start|>assistant\n";
        if (!preserve_thinking && !rewrite_checkpoint) {
            rewrite_checkpoint = RewriteCheckpointByteSpec{
                .kind = RewriteCheckpointKind::TurnClosure, .offset = rendered.size()};
        }
        if (options.enable_thinking) {
            rendered += "<think>\n";
        } else {
            rendered += "<think>\n\n</think>\n\n";
        }
        if (preserve_thinking) {
            // Response replay retains the deterministic generation prologue. This is the prompt
            // frontier for both thinking modes, so capturing it does not split off a tiny final
            // prefill unit. The complete rendered prefix is tokenized independently below.
            rewrite_checkpoint = RewriteCheckpointByteSpec{
                .kind = RewriteCheckpointKind::ResponseReplay, .offset = rendered.size()};
        }
    }
    return RenderedChat{.text = std::move(rendered), .rewrite_checkpoint = rewrite_checkpoint};
}

} // namespace ninfer::targets::qwen3_6::frontend_internal
