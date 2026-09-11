#include "targets/qwen3_6/impl/frontend/chat_template.h"

#include "targets/qwen3_6/impl/frontend/digest.h"
#include "targets/qwen3_6/impl/frontend/jinja_chat_render.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ninfer::targets::qwen3_6::frontend_internal {

namespace {

constexpr Sha256Digest kThinkingToggleTemplateDigest{
    0xe8, 0x4f, 0x32, 0xa2, 0x3f, 0xdd, 0xa2, 0x76, 0x89, 0xf8, 0x68, 0xaa, 0x4a, 0x1a, 0x56, 0x21,
    0xf4, 0x11, 0x33, 0xe5, 0x1a, 0x48, 0xd7, 0xf3, 0xef, 0xcb, 0xea, 0x28, 0x39, 0x57, 0x42, 0x59,
};

constexpr Sha256Digest kReasoningEffortTemplateDigest{
    0xc3, 0xcf, 0x9e, 0x34, 0xab, 0xf4, 0xf9, 0xe3, 0x6c, 0x2d, 0x72, 0x16, 0x5a, 0xa9, 0xc1, 0x32,
    0xd3, 0xe2, 0xa7, 0x25, 0xb6, 0xc2, 0x58, 0x6a, 0xaa, 0x3a, 0x8a, 0xf9, 0xd7, 0xa8, 0x10, 0x41,
};

// qwen3.8-froggeric-v22.5, locally extended with the no-dangling-intent tool rule.
constexpr Sha256Digest kFroggericV225TemplateDigest{
    0x7b, 0x79, 0x6c, 0xb1, 0x95, 0x34, 0x63, 0x23, 0x1d, 0x87, 0x57, 0xa4, 
    0xa2, 0x3a, 0xb6, 0x89, 0x2b, 0x8b, 0xaf, 0xaa, 0x61, 0xb1, 0xac, 0xc4, 
    0x7e, 0x16, 0xbf, 0xbb, 0x7c, 0x17, 0xeb, 0x9d
};

// The template the currently deployed qwen3.8 artifact embeds (froggeric v22). It is
// accepted until that artifact is rebuilt from the v22.5 fixture, then it goes away.
constexpr Sha256Digest kFroggericV22DeployedTemplateDigest{
    0x39, 0x8e, 0xdf, 0x5b, 0x5b, 0xb8, 0x02, 0xfb, 0x6b, 0x9c, 0x9a, 0x8d, 0xba, 0x67, 0x0d, 0x09,
    0xf2, 0xaa, 0xee, 0xf6, 0xfd, 0xca, 0xa0, 0xb2, 0xca, 0x30, 0x72, 0x65, 0xf5, 0x9f, 0x78, 0xdc,
};


} // namespace

bool ChatMessage::has_media() const noexcept {
    for (const ChatPart& part : parts) {
        if (part.kind != ChatPartKind::Text) { return true; }
    }
    return false;
}

CompiledChatTemplate::CompiledChatTemplate(std::shared_ptr<const JinjaChatTemplate> compiled,
                                           ChatTemplateSemantics semantics) noexcept
    : compiled_(std::move(compiled)), semantics_(semantics) {}

CompiledChatTemplate CompiledChatTemplate::resolve(std::string_view source) {
    const Sha256Digest digest = sha256(source);
    ChatTemplateSemantics semantics;
    if (digest == kThinkingToggleTemplateDigest) {
        semantics = ChatTemplateSemantics::ThinkingToggle;
    } else if (digest == kReasoningEffortTemplateDigest) {
        semantics = ChatTemplateSemantics::ReasoningEffort;
    } else if (digest == kFroggericV225TemplateDigest ||
               digest == kFroggericV22DeployedTemplateDigest) {
        semantics = ChatTemplateSemantics::FroggericV22;
    } else {
        throw std::invalid_argument("unsupported frontend/chat_template.jinja (sha256 " +
                                    sha256_hex(digest) + ")");
    }
    return CompiledChatTemplate(
        std::make_shared<const JinjaChatTemplate>(
            JinjaChatTemplate::compile(std::string(source))),
        semantics);
}

CompiledChatTemplate CompiledChatTemplate::resolve_with_semantics(
    std::string_view source, std::string_view semantics_name) {
    if (semantics_name == "auto") { return resolve(source); }
    ChatTemplateSemantics semantics;
    if (semantics_name == "froggeric") {
        semantics = ChatTemplateSemantics::FroggericV22;
    } else if (semantics_name == "thinking-toggle") {
        semantics = ChatTemplateSemantics::ThinkingToggle;
    } else if (semantics_name == "reasoning-effort") {
        semantics = ChatTemplateSemantics::ReasoningEffort;
    } else if (semantics_name == "generic") {
        // Any template is accepted; the capabilities of a thinking-toggle model are assumed.
        semantics = ChatTemplateSemantics::ThinkingToggle;
    } else {
        throw std::invalid_argument("unknown --chat-template-semantics '" +
                                    std::string(semantics_name) +
                                    "'; use auto, froggeric, thinking-toggle, reasoning-effort, "
                                    "or generic");
    }
    return CompiledChatTemplate(
        std::make_shared<const JinjaChatTemplate>(
            JinjaChatTemplate::compile(std::string(source))),
        semantics);
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

const std::string& CompiledChatTemplate::source() const noexcept { return compiled_->source(); }

RenderedChat CompiledChatTemplate::render(const std::vector<ChatMessage>& messages,
                                          ChatRenderOptions options) const {
    return compiled_->render(messages, options);
}

} // namespace ninfer::targets::qwen3_6::frontend_internal
