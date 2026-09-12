// Validates that the structured render description keeps every media placeholder a
// registered template emits, whatever position the image-bearing message holds.
//
// The frontend expands one placeholder per input image; a lost placeholder rejects the
// request with "chat media count does not match rendered placeholders", so a mapping
// that only holds for an image in the first message breaks vision for every later turn
// (an image after a tool result, or after any earlier exchange).
//
// The digest-registered template the deployed qwen3.8 artifact embeds is one of the
// files exercised here. The render oracle suite covers the same template shape, but none
// of its scenarios places an image after the first message, which is why this mapping bug
// escaped it.

#include "targets/qwen3_6/impl/frontend/chat_template.h"
#include "targets/qwen3_6/impl/frontend/jinja_chat_render.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fi = ninfer::targets::qwen3_6::frontend_internal;

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

fi::ChatMessage text_message(ninfer::ChatRole role, const std::string& text) {
    fi::ChatMessage message;
    message.role = role;
    message.parts.push_back(fi::ChatPart::text_part(text));
    return message;
}

fi::ChatMessage image_message(ninfer::ChatRole role, const std::string& text) {
    fi::ChatMessage message = text_message(role, text);
    fi::ChatPart part;
    part.kind = fi::ChatPartKind::Image;
    message.parts.push_back(std::move(part));
    return message;
}

fi::ChatMessage tool_call_message(const std::string& name) {
    fi::ChatMessage message = text_message(ninfer::ChatRole::Assistant, "");
    fi::ToolCall call;
    call.id             = "call-1";
    call.name           = name;
    call.arguments_json = R"({"path":"shot.png"})";
    message.tool_calls.push_back(std::move(call));
    return message;
}

std::size_t image_parts(const std::vector<fi::ChatMessage>& messages) {
    std::size_t count = 0;
    for (const fi::ChatMessage& message : messages) {
        for (const fi::ChatPart& part : message.parts) {
            if (part.kind == fi::ChatPartKind::Image) { ++count; }
        }
    }
    return count;
}

// One placeholder per image, in input order, each covering the image pad token it stands for.
int check_media_mapping(const std::string& label, const std::vector<fi::ChatMessage>& messages,
                        const fi::RenderedChat& rendered) {
    const std::size_t images = image_parts(messages);
    if (rendered.media_placeholders.size() != images) {
        return fail(label + ": " + std::to_string(images) + " images mapped onto " +
                    std::to_string(rendered.media_placeholders.size()) + " placeholders");
    }
    int failures = 0;
    for (std::size_t index = 0; index < rendered.media_placeholders.size(); ++index) {
        const fi::MediaPlaceholderByteSpec& placeholder = rendered.media_placeholders[index];
        if (placeholder.item_index != index) {
            failures += fail(label + ": placeholder " + std::to_string(index) +
                             " is out of input order");
            break;
        }
        if (placeholder.bytes.end > rendered.text.size() ||
            rendered.text.substr(placeholder.bytes.begin,
                                 placeholder.bytes.end - placeholder.bytes.begin) !=
                "<|image_pad|>") {
            failures += fail(label + ": placeholder " + std::to_string(index) +
                             " does not cover the image pad token");
            break;
        }
    }
    return failures;
}

struct Shape {
    const char* name;
    std::vector<fi::ChatMessage> messages;
};

} // namespace

int main() {
    const std::string fixture_dir = NINFER_SOURCE_DIR "/tests/fixtures/frontend/";
    const char* templates[] = {
        "froggeric_v225_chat_template.jinja",
        // The template the deployed qwen3.8 artifact embeds.
        "qwen38_reasoning_effort_deployed_chat_template.jinja",
    };

    std::vector<Shape> shapes;
    shapes.push_back({"first-message-image", {image_message(ninfer::ChatRole::User, "look: ")}});
    shapes.push_back({"image-then-turn",
                      {image_message(ninfer::ChatRole::User, "look: "),
                       text_message(ninfer::ChatRole::Assistant, "a red square"),
                       text_message(ninfer::ChatRole::User, "and now?")}});
    // What a client sends on the turn after any earlier exchange.
    shapes.push_back({"image-after-turn",
                      {text_message(ninfer::ChatRole::User, "hi"),
                       text_message(ninfer::ChatRole::Assistant, "hello"),
                       image_message(ninfer::ChatRole::User, "look: ")}});
    // What a client sends when a tool returns an image: the tool result carries it, or the
    // user turn that follows the result does.
    shapes.push_back({"image-in-tool-result",
                      {text_message(ninfer::ChatRole::User, "read shot.png"),
                       tool_call_message("read"),
                       image_message(ninfer::ChatRole::Tool, "shot.png: "),
                       text_message(ninfer::ChatRole::User, "what is it?")}});
    shapes.push_back({"image-after-tool-result",
                      {text_message(ninfer::ChatRole::User, "read shot.png"),
                       tool_call_message("read"),
                       text_message(ninfer::ChatRole::Tool, "shot.png: 64x64 png"),
                       image_message(ninfer::ChatRole::User, "look: ")}});
    shapes.push_back({"images-in-two-turns",
                      {image_message(ninfer::ChatRole::User, "first: "),
                       text_message(ninfer::ChatRole::Assistant, "seen"),
                       image_message(ninfer::ChatRole::User, "second: ")}});
    // Two images in one LATE message: the content macro's loop over that message's parts
    // runs twice inside that iteration, which splits the message loop's records. Both
    // details are load-bearing - a first-message image is mapped through the preamble path
    // and a single-image message runs the parts loop once, so neither shape reaches this.
    {
        fi::ChatMessage both;
        both.role = ninfer::ChatRole::User;
        both.parts.push_back(fi::ChatPart::text_part("both: "));
        for (int index = 0; index < 2; ++index) {
            fi::ChatPart image;
            image.kind = fi::ChatPartKind::Image;
            both.parts.push_back(std::move(image));
        }
        shapes.push_back({"two-images-one-late-message",
                          {text_message(ninfer::ChatRole::User, "hi"),
                           text_message(ninfer::ChatRole::Assistant, "hello"),
                           std::move(both)}});
    }

    int failures = 0;
    int checked  = 0;
    for (const char* file : templates) {
        const fi::JinjaChatTemplate compiled =
            fi::JinjaChatTemplate::compile(read_file(fixture_dir + file));
        for (const Shape& shape : shapes) {
            const std::string label = std::string(file) + "/" + shape.name;
            fi::ChatRenderOptions options;
            options.add_generation_prompt = true;
            fi::RenderedChat rendered;
            try {
                rendered = compiled.render(shape.messages, options);
            } catch (const std::exception& error) {
                failures += fail(label + " threw: " + error.what());
                continue;
            }
            failures += check_media_mapping(label, shape.messages, rendered);
            // A lost message loop drops every per-message frontier along with the
            // placeholders, so the boundaries are part of the same contract.
            for (std::size_t index = 1; index <= shape.messages.size(); ++index) {
                if (!rendered.message_boundaries[index]) {
                    failures += fail(label + ": message boundary " + std::to_string(index) +
                                     " was not mapped");
                    break;
                }
            }
            ++checked;
        }
    }
    if (failures == 0) { std::cout << "ok (" << checked << " renders)\n"; }
    return failures == 0 ? 0 : 1;
}
