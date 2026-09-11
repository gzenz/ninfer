#include "targets/qwen3_6/impl/frontend/jinja_chat_render.h"

#include "jinja/jinja.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ninfer::targets::qwen3_6::frontend_internal {
namespace {

using json = jinja::json;

constexpr std::string_view kAssistantOpener = "<|im_start|>assistant\n";
constexpr std::string_view kVisionStart     = "<|vision_start|>";
constexpr std::string_view kVisionEnd       = "<|vision_end|>";
constexpr std::string_view kImagePad        = "<|image_pad|>";
constexpr std::string_view kVideoPad        = "<|video_pad|>";

std::string_view role_name(ChatRole role) {
    switch (role) {
    case ChatRole::System: return "system";
    case ChatRole::Developer: return "developer";
    case ChatRole::User: return "user";
    case ChatRole::Assistant: return "assistant";
    case ChatRole::Tool: return "tool";
    }
    throw std::invalid_argument("unsupported chat role value");
}

std::string_view effort_name(ReasoningEffort effort) noexcept {
    switch (effort) {
    case ReasoningEffort::Low: return "low";
    case ReasoningEffort::Medium: return "medium";
    case ReasoningEffort::XHigh: return "xhigh";
    }
    return "xhigh";
}

// The wire carries tool arguments as a JSON string. The template renders parameter
// blocks from a mapping and passes a string through verbatim, so parse when possible.
json tool_arguments(const std::string& text) {
    if (text.empty()) { return json::object(); }
    try {
        json parsed = json::parse(text);
        if (parsed.is_object()) { return parsed; }
    } catch (const std::exception&) {
    }
    return text;
}

std::string message_text(const ChatMessage& message) {
    std::string text;
    for (const ChatPart& part : message.parts) {
        if (part.kind == ChatPartKind::Text) { text += part.text; }
    }
    return text;
}

json build_context(const std::vector<ChatMessage>& messages, const ChatRenderOptions& options) {
    json context = json::object();
    json rendered_messages = json::array();
    for (const ChatMessage& message : messages) {
        json entry = json::object();
        entry["role"] = std::string(role_name(message.role));
        if (message.has_media()) {
            json content_parts = json::array();
            for (const ChatPart& part : message.parts) {
                json item = json::object();
                switch (part.kind) {
                case ChatPartKind::Text:
                    item["type"] = "text";
                    item["text"] = part.text;
                    break;
                case ChatPartKind::Image:
                    item["type"] = "image";
                    break;
                case ChatPartKind::Video:
                    item["type"] = "video";
                    break;
                }
                content_parts.push_back(std::move(item));
            }
            entry["content"] = std::move(content_parts);
        } else {
            entry["content"] = message_text(message);
        }
        if (!message.reasoning_content.empty()) {
            entry["reasoning_content"] = message.reasoning_content;
        }
        if (!message.tool_calls.empty()) {
            json calls = json::array();
            for (const ToolCall& call : message.tool_calls) {
                json entry_call = json::object();
                entry_call["id"] = call.id;
                entry_call["type"] = "function";
                json function = json::object();
                function["name"] = call.name;
                function["arguments"] = tool_arguments(call.arguments_json);
                entry_call["function"] = std::move(function);
                calls.push_back(std::move(entry_call));
            }
            entry["tool_calls"] = std::move(calls);
        }
        rendered_messages.push_back(std::move(entry));
    }
    context["messages"] = std::move(rendered_messages);
    context["enable_thinking"] = options.enable_thinking;
    context["add_generation_prompt"] = options.add_generation_prompt;
    if (options.preserve_thinking) { context["preserve_thinking"] = *options.preserve_thinking; }
    if (options.reasoning_effort) {
        context["reasoning_effort"] = std::string(effort_name(*options.reasoning_effort));
    }
    if (options.add_vision_id) { context["add_vision_id"] = true; }
    if (!options.tool_jsons.empty()) {
        json tools = json::array();
        for (const std::string& tool : options.tool_jsons) { tools.push_back(json::parse(tool)); }
        context["tools"] = std::move(tools);
    }
    return context;
}

// A run of consecutive trace iterations belonging to one loop statement.
struct LoopRun {
    std::string loop;
    std::size_t begin = 0;
    std::size_t count = 0;
};

std::vector<LoopRun> loop_runs(const jinja::RenderTrace& trace) {
    std::vector<LoopRun> runs;
    for (std::size_t index = 0; index < trace.loop_iterations.size(); ++index) {
        const std::string& loop = trace.loop_iterations[index].loop;
        if (!runs.empty() && runs.back().loop == loop &&
            runs.back().begin + runs.back().count == index) {
            ++runs.back().count;
            continue;
        }
        runs.push_back(LoopRun{loop, index, 1U});
    }
    return runs;
}

// The loop that emits one block per input message. It is identified by emitted bytes and
// role order rather than by variable spelling, so a renamed loop variable does not break
// the mapping.
struct MessageLoop {
    std::size_t begin = 0;
    std::size_t count = 0;
    std::size_t offset = 0; // input messages folded into the preamble
};

std::optional<MessageLoop> select_message_loop(const jinja::RenderTrace& trace,
                                               const std::vector<ChatMessage>& messages) {
    for (const LoopRun& run : loop_runs(trace)) {
        std::size_t emitted = 0;
        bool contiguous = true;
        for (std::size_t index = 0; index < run.count; ++index) {
            const auto& iteration = trace.loop_iterations[run.begin + index];
            emitted += iteration.end - iteration.begin;
            if (index != 0 &&
                trace.loop_iterations[run.begin + index - 1U].end != iteration.begin) {
                contiguous = false;
            }
        }
        if (emitted == 0 || !contiguous) { continue; }
        for (const std::size_t offset : {std::size_t{0}, std::size_t{1}}) {
            if (run.count + offset != messages.size()) { continue; }
            bool matches = true;
            for (std::size_t index = 0; index < run.count; ++index) {
                if (trace.loop_iterations[run.begin + index].item_role !=
                    role_name(messages[index + offset].role)) {
                    matches = false;
                    break;
                }
            }
            if (matches) { return MessageLoop{run.begin, run.count, offset}; }
        }
    }
    return std::nullopt;
}

struct Leaf {
    std::size_t begin = 0;
    std::size_t end = 0;
};

// Finds the printed leaf named `expr` inside [begin, end) and returns its byte span.
std::optional<Leaf> find_leaf(const jinja::RenderTrace& trace, std::size_t begin, std::size_t end,
                              std::string_view expr) {
    for (const auto& print : trace.prints) {
        if (print.begin < begin || print.end > end) { continue; }
        std::size_t offset = print.begin;
        for (const auto& part : print.parts) {
            if (part.expr == expr) { return Leaf{offset, offset + part.length}; }
            offset += part.length;
        }
    }
    return std::nullopt;
}

void append_literal_span(std::vector<ByteSpan>& spans, ByteSpan span) {
    if (span.begin == span.end) { return; }
    if (!spans.empty() && spans.back().end == span.begin) {
        spans.back().end = span.end;
        return;
    }
    if (!spans.empty() && spans.back().end > span.begin) {
        throw std::logic_error("rendered literal byte spans overlap");
    }
    spans.push_back(span);
}

// Longest realistic tail of tool output that a truncated import may start at.
bool looks_like_tool_response(std::string_view text) {
    const auto [begin, end] = jinja::python_strip_bounds(std::string(text));
    text = text.substr(begin, end - begin);
    return text.starts_with("<tool_response>") && text.ends_with("</tool_response>");
}

long last_real_user_query(const std::vector<ChatMessage>& messages) {
    long trailing_tool_query = -1;
    for (long index = static_cast<long>(messages.size()) - 1; index >= 0; --index) {
        const ChatMessage& message = messages[static_cast<std::size_t>(index)];
        if (message.role == ChatRole::Tool && trailing_tool_query < 0) {
            trailing_tool_query = index;
        }
        if (message.role != ChatRole::User) { continue; }
        if (!looks_like_tool_response(message_text(message))) { return index; }
    }
    if (trailing_tool_query >= 0) { return trailing_tool_query; }
    throw std::invalid_argument("no user query found in chat messages");
}

} // namespace

// Rebuilds the content string the template composes for a message, then walks it to
// locate caller text and media placeholders exactly. Template and frontend must agree on
// the content shape; a mismatch is reported instead of emitting wrong byte spans, because
// those spans decide where vision embeddings are inserted.
struct ContentLayout {
    std::vector<ByteSpan> literals;
    std::vector<MediaPlaceholderByteSpec> media;
    std::vector<std::size_t> part_offsets; // absolute frontier after each input part
};

ContentLayout derive_content_layout(const ChatMessage& message, Leaf leaf, const std::string& text,
                                    bool add_vision_id, std::size_t& media_index) {
    ContentLayout layout;
    const std::size_t leaf_length = leaf.end - leaf.begin;
    if (!message.has_media()) {
        layout.literals.push_back(ByteSpan{leaf.begin, leaf.end});
        std::size_t cursor = 0;
        for (const ChatPart& part : message.parts) {
            if (part.kind == ChatPartKind::Text) { cursor += part.text.size(); }
            layout.part_offsets.push_back(leaf.begin + std::min(cursor, leaf_length));
        }
        return layout;
    }

    std::string reconstruction;
    struct Piece {
        bool media = false;
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t pad_offset = 0; // offset of the pad token inside the piece
        std::size_t pad_length = 0;
        Modality modality = Modality::Image;
    };
    std::vector<Piece> pieces;
    int image_label = 0;
    int video_label = 0;
    for (const ChatPart& part : message.parts) {
        Piece piece;
        piece.begin = reconstruction.size();
        if (part.kind == ChatPartKind::Text) {
            reconstruction += part.text;
        } else {
            const bool image = part.kind == ChatPartKind::Image;
            piece.media    = true;
            piece.modality = image ? Modality::Image : Modality::Video;
            if (add_vision_id) {
                reconstruction += image ? "Picture " : "Video ";
                reconstruction += std::to_string(image ? ++image_label : ++video_label);
                reconstruction += ": ";
            }
            reconstruction += kVisionStart;
            piece.pad_offset = reconstruction.size();
            piece.pad_length = (image ? kImagePad : kVideoPad).size();
            reconstruction += image ? kImagePad : kVideoPad;
            reconstruction += kVisionEnd;
        }
        piece.end = reconstruction.size();
        pieces.push_back(piece);
    }

    const auto [trim_begin, trim_end] = jinja::python_strip_bounds(reconstruction);
    const std::string expected = reconstruction.substr(trim_begin, trim_end - trim_begin);
    if (expected != text.substr(leaf.begin, leaf_length)) {
        throw std::runtime_error(
            "rendered message content does not match the message parts; the template "
            "transformed the content in a way the frontend cannot map");
    }

    const auto to_absolute = [&](std::size_t offset) {
        return leaf.begin + (offset - trim_begin);
    };
    for (const Piece& piece : pieces) {
        const std::size_t visible_begin = std::max(piece.begin, trim_begin);
        const std::size_t visible_end   = std::min(piece.end, trim_end);
        if (visible_begin < visible_end && !piece.media) {
            layout.literals.push_back(ByteSpan{to_absolute(visible_begin), to_absolute(visible_end)});
        }
        if (piece.media) {
            const std::size_t pad_begin = piece.pad_offset;
            const std::size_t pad_end   = piece.pad_offset + piece.pad_length;
            if (pad_begin >= trim_begin && pad_end <= trim_end) {
                layout.media.push_back(MediaPlaceholderByteSpec{
                    .bytes      = ByteSpan{to_absolute(pad_begin), to_absolute(pad_end)},
                    .modality   = piece.modality,
                    .item_index = media_index,
                });
            }
            ++media_index;
        }
        layout.part_offsets.push_back(
            visible_begin < visible_end ? to_absolute(visible_end)
                                        : (piece.begin < trim_begin ? leaf.begin : leaf.end));
    }
    return layout;
}

struct JinjaChatTemplate::Impl {
    jinja::Template template_;
    std::string source;

    explicit Impl(std::string value)
        : template_(value), source(std::move(value)) {}
};

JinjaChatTemplate::JinjaChatTemplate(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

JinjaChatTemplate JinjaChatTemplate::compile(std::string source) {
    if (source.empty()) { throw std::invalid_argument("chat template source is empty"); }
    auto impl = std::make_shared<Impl>(std::move(source));
    impl->template_.add_function("raise_exception",
                                 [](const std::vector<jinja::Argument>& args) -> jinja::json {
                                     std::string message;
                                     if (!args.empty() && args[0].second.is_string()) {
                                         message = args[0].second.get<std::string>();
                                     }
                                     throw std::invalid_argument(message.empty() ? "template raised"
                                                                                 : message);
                                 });
    return JinjaChatTemplate(std::move(impl));
}

const std::string& JinjaChatTemplate::source() const noexcept { return impl_->source; }

RenderedChat JinjaChatTemplate::render(const std::vector<ChatMessage>& messages,
                                       const ChatRenderOptions& options) const {
    if (messages.empty()) { throw std::invalid_argument("chat messages must not be empty"); }
    const json context = build_context(messages, options);

    jinja::RenderTrace trace;
    RenderedChat rendered;
    rendered.text = impl_->template_.render(context, &trace);

    rendered.message_boundaries.assign(messages.size() + 1U, std::nullopt);
    rendered.cache_boundaries.assign(options.cache_markers.size(), std::nullopt);
    const std::optional<MessageLoop> loop = select_message_loop(trace, messages);
    if (!loop) { return rendered; }

    const auto& iterations = trace.loop_iterations;
    const std::size_t loop_begin = iterations[loop->begin].begin;
    const std::size_t loop_end   = iterations[loop->begin + loop->count - 1U].end;
    // A leading instruction message is folded into the preamble by the template, and the
    // loop then has no emitted block for it. Such a message has no independent frontier, so
    // only the frontier that follows it is recorded.
    const bool first_iteration_emits = iterations[loop->begin].end > iterations[loop->begin].begin;
    rendered.message_boundaries[loop->offset] =
        first_iteration_emits ? std::optional<std::size_t>(loop_begin) : std::nullopt;
    for (std::size_t index = first_iteration_emits ? 0U : 1U; index < loop->count; ++index) {
        rendered.message_boundaries[loop->offset + index + 1U] = iterations[loop->begin + index].end;
    }
    if (!first_iteration_emits) {
        // The folded message's frontier is the preamble end, which is where the loop starts.
        rendered.message_boundaries[loop->offset + 1U] = loop_begin;
    }

    // A folded instruction message is rendered in the system preamble, outside the loop; its
    // text is still caller-provided and therefore literal.
    std::optional<std::size_t> instruction_begin;
    std::size_t instruction_length = 0;
    // Message 0 is folded when the loop starts after it, or when its own iteration emits
    // nothing because the template rendered it in the preamble.
    const bool instruction_folded = loop->offset == 1U || !first_iteration_emits;
    if (instruction_folded) {
        // The folded instruction's value is bound to different locals across the registered
        // templates, so both spellings are accepted.
        std::optional<Leaf> instruction_leaf = find_leaf(trace, 0, loop_begin, "_sc");
        if (!instruction_leaf) { instruction_leaf = find_leaf(trace, 0, loop_begin, "content"); }
        if (const auto& leaf = instruction_leaf) {
            append_literal_span(rendered.literal_spans, ByteSpan{leaf->begin, leaf->end});
            instruction_begin  = leaf->begin;
            instruction_length = leaf->end - leaf->begin;
        }
    }

    std::size_t media_index = 0;
    std::vector<std::vector<std::size_t>> part_offsets(messages.size());
    for (std::size_t index = 0; index < loop->count; ++index) {
        const auto& iteration = iterations[loop->begin + index];
        const std::size_t message_index = loop->offset + index;
        const ChatMessage& message = messages[message_index];
        if (const auto leaf = find_leaf(trace, iteration.begin, iteration.end, "reasoning_content")) {
            append_literal_span(rendered.literal_spans, ByteSpan{leaf->begin, leaf->end});
        }
        const auto leaf = find_leaf(trace, iteration.begin, iteration.end, "content");
        if (leaf) {
            ContentLayout layout = derive_content_layout(message, *leaf, rendered.text,
                                                         options.add_vision_id, media_index);
            for (const ByteSpan span : layout.literals) {
                append_literal_span(rendered.literal_spans, span);
            }
            for (const MediaPlaceholderByteSpec& placeholder : layout.media) {
                rendered.media_placeholders.push_back(placeholder);
            }
            part_offsets[message_index] = std::move(layout.part_offsets);
        }
        if (message.role == ChatRole::Assistant) {
            // A partially generated assistant turn can be rewritten, and the template's own
            // block literals mark where a continuation may resume.
            const bool final_continuation =
                options.continuation == PromptContinuationMode::ContinueFinalAssistant &&
                message_index + 1U == messages.size();
            if (final_continuation) {
                rendered.rewrite_execution_boundaries.push_back(iteration.begin +
                                                               kAssistantOpener.size());
            } else {
                // The template emits the reasoning block as one literal, so the intermediate
                // frontiers come from the leaves rather than from part boundaries.
                rendered.rewrite_execution_boundaries.push_back(iteration.begin +
                                                               kAssistantOpener.size());
                const auto reasoning =
                    find_leaf(trace, iteration.begin, iteration.end, "reasoning_content");
                if (reasoning && leaf) {
                    rendered.rewrite_execution_boundaries.push_back(reasoning->begin);
                    rendered.rewrite_execution_boundaries.push_back(leaf->begin);
                }
            }
        }
    }

    // Tool definitions are emitted by a loop over the tool list, so their frontiers come
    // from the trace as well.
    std::vector<std::size_t> tool_boundaries;
    for (const LoopRun& run : loop_runs(trace)) {
        if (run.loop != "tools" || run.count == 0) { continue; }
        if (iterations[run.begin].begin < loop_end) { continue; }
        for (std::size_t index = 0; index < run.count; ++index) {
            tool_boundaries.push_back(iterations[run.begin + index].end);
        }
    }

    for (std::size_t index = 0; index < options.cache_markers.size(); ++index) {
        const PromptCacheMarker marker = options.cache_markers[index];
        switch (marker.location) {
        case PromptCacheMarkerLocation::MessageBoundary:
            if (marker.after_message_count < rendered.message_boundaries.size()) {
                rendered.cache_boundaries[index] = rendered.message_boundaries[marker.after_message_count];
            }
            break;
        case PromptCacheMarkerLocation::MessagePartBoundary:
            if (marker.after_message_count == 0 ||
                marker.after_message_count > messages.size() ||
                marker.after_message_part_count == 0) {
                break;
            }
            {
                const std::size_t message_index = marker.after_message_count - 1U;
                const std::vector<std::size_t>& offsets = part_offsets[message_index];
                if (marker.after_message_part_count <= offsets.size()) {
                    rendered.cache_boundaries[index] = offsets[marker.after_message_part_count - 1U];
                }
            }
            break;
        case PromptCacheMarkerLocation::LeadingInstructionBoundary:
            if (instruction_begin && marker.leading_instruction_bytes <= instruction_length) {
                rendered.cache_boundaries[index] =
                    *instruction_begin + marker.leading_instruction_bytes;
            }
            break;
        case PromptCacheMarkerLocation::ToolBoundary:
            if (marker.after_tool_count != 0 && marker.after_tool_count <= tool_boundaries.size()) {
                rendered.cache_boundaries[index] = tool_boundaries[marker.after_tool_count - 1U];
            }
            break;
        }
    }

    const bool preserve_thinking = options.preserve_thinking.value_or(true);
    long last_query = 0;
    try {
        last_query = last_real_user_query(messages);
    } catch (const std::invalid_argument&) {
        last_query = messages.size() > 50U ? static_cast<long>(messages.size()) - 1 : 0;
    }

    if (options.continuation == PromptContinuationMode::ContinueFinalAssistant && loop->count != 0) {
        // The final assistant turn is replayed and may be rewritten as the response continues.
        const auto& iteration = iterations[loop->begin + loop->count - 1U];
        rendered.rewrite_checkpoint =
            RewriteCheckpointByteSpec{RewriteCheckpointKind::ResponseReplay, iteration.begin};
    } else if (options.add_generation_prompt) {
        // The generation suffix is replaceable as a unit; its own prints give the prologue
        // frontiers.
        std::optional<std::size_t> suffix_begin;
        for (const auto& print : trace.prints) {
            if (print.begin < loop_end || print.parts.size() != 1U) { continue; }
            const auto& part = print.parts.front();
            if (!part.expr.empty() && part.expr.front() != '"') { continue; }
            if (!suffix_begin) { suffix_begin = print.begin; }
            if (print.end > print.begin) { rendered.rewrite_execution_boundaries.push_back(print.end); }
        }
        if (suffix_begin) {
            rendered.rewrite_checkpoint = RewriteCheckpointByteSpec{
                preserve_thinking ? RewriteCheckpointKind::ResponseReplay
                                  : RewriteCheckpointKind::TurnClosure,
                *suffix_begin};
        }
    } else if (!preserve_thinking) {
        for (std::size_t index = 0; index < loop->count; ++index) {
            const std::size_t message_index = loop->offset + index;
            if (messages[message_index].role != ChatRole::Assistant) { continue; }
            if (static_cast<long>(message_index) <= last_query) { continue; }
            const auto& iteration = iterations[loop->begin + index];
            rendered.rewrite_checkpoint =
                RewriteCheckpointByteSpec{RewriteCheckpointKind::TurnClosure, iteration.begin};
        }
    }
    return rendered;
}

} // namespace ninfer::targets::qwen3_6::frontend_internal
