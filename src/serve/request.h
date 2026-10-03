#pragma once

#include "product/media_acquire/source.h"

#include <ninfer/types.h>

// Internal, wire-format-independent representation of a generation request.
//
// OpenAI and Anthropic schemas map into this wire-independent value.
// translate.cpp then produces the public PromptInput and RequestOptions consumed
// by Engine; media sources remain unresolved until the product service acquires
// owning bytes.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ninfer::serve {

// A structured API error mapped onto an error object + HTTP status. Wire-format
// independent: each protocol layer renders it into its own error body shape.
struct ApiError {
    int status       = 400;
    std::string type = "invalid_request_error";
    std::string message;
    std::string param; // optional
    std::string code;  // optional
};

class ApiException : public std::runtime_error {
public:
    explicit ApiException(ApiError error)
        : std::runtime_error(error.message), error_(std::move(error)) {}

    [[nodiscard]] const ApiError& error() const noexcept { return error_; }

private:
    ApiError error_;
};

// Server-side context needed while parsing/validating a request.
struct RequestLimits {
    int default_max_tokens = 8192;
};

enum class ContentKind {
    Text,
    Image,
    Video,
};

struct CacheBoundary {
    enum class Ttl : std::uint8_t {
        Default,
        FiveMinutes,
        OneHour,
    };

    ninfer::PromptCacheMarkerKind kind       = ninfer::PromptCacheMarkerKind::SharedStablePrefix;
    ninfer::SharedCandidateEvidence evidence = ninfer::SharedCandidateEvidence::ExplicitBoundary;
    Ttl ttl                                  = Ttl::Default;

    [[nodiscard]] friend constexpr bool operator==(CacheBoundary, CacheBoundary) noexcept = default;
};

struct ContentPart {
    ContentKind kind = ContentKind::Text;
    std::string text;     // populated for Text
    std::string type_raw; // original wire "type" string for diagnostics
    ninfer::product::media_acquire::Source source;
    ninfer::ImageResizePolicy image_resize_policy = ninfer::ImageResizePolicy::Downsize;
    std::optional<CacheBoundary> cache_boundary_after;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    std::string input_schema_json;
    std::optional<std::string> input_examples_json;
    std::optional<CacheBoundary> cache_boundary_after;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

enum class ToolChoiceMode {
    Auto,
    None,
};

struct ToolChoice {
    ToolChoiceMode mode = ToolChoiceMode::Auto;
};

struct ChatTurn {
    ChatRole role = ChatRole::User;
    std::vector<ContentPart> content; // ordered parts; may be empty when wire content is empty
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id; // populated for role=tool
    // Optional protocol assertion for a tool result. Call-graph normalization verifies it against
    // the function identified by tool_call_id before the Engine sees the history.
    std::optional<std::string> tool_result_name;
    bool tool_result_is_error = false;
    std::string reasoning_content; // assistant thinking carried across turns (round-tripped to the
                                   // template)
    std::optional<CacheBoundary> cache_boundary_after;
};

// DERIVE A SESSION KEY FOR REQUESTS THAT CARRY NONE -- the V2 fix (613171bd, 2026-09-18) PORTED TO V3.
//
// WHY IT IS NEEDED, in V2's own words, because the V3 rewrite reproduced the condition it describes: "the
// session key is a client-provided hint that only the OpenAI Responses path populates; the anthropic
// Messages handler never does." V3 today: `ContextCacheHints` on the Messages path is default-constructed,
// so `out.session_key` is empty, and with it
//   * `reuse_domain()` falls back to `publication_order` -- PER REQUEST -- so no request can ever share a
//     reuse domain with an earlier turn of its own conversation, and
//   * retention is chosen `RecentPrivate` instead of `LiveSession` (`frontend.cpp:434`), so a live session's
//     units are not protected in the catalog.
// Measured on prod 2026-09-27 before this: 559 distinct incoming identities against 74 stored ones, and every
// private-candidate refusal labelled XSESSION-MISMATCH -- a label the candgen guard cannot make meaningful
// without a key, since its `own_session` test requires one on BOTH sides.
//
// THE PREIMAGE IS V2'S, unchanged: FNV-1a (the net's own constants) over the system-prompt text plus the
// FIRST user turn's text, with a unit separator so a system/user boundary is unambiguous. The system prompt
// is constant within a deployment and the first user turn is fixed for the life of a conversation, so the key
// is stable across its turns and changes after a client compaction -- correct, because pre-compaction units
// no longer match the post-compaction prompt. A conversation with no user text (an image-only first turn)
// gets NO key and keeps the pre-fix behaviour rather than getting an arbitrary one.
// THE OTHER HALF OF A JOIN the engine side has been waiting for. `prefill.cpp` hashes the session key onto
// the sequence (`session_key_hash`, with 0 remapped to 1 so "no key" is distinguishable from a hash that came
// out zero), and the eviction line prints it as `session=%016llx`. The request log recorded no matching field,
// so an eviction could not be tied to the request whose state it destroyed -- which is exactly the link that
// would say whether #6's evictions are what caps reuse at ~31k. This computes the SAME hash over the SAME
// bytes, so the two logs can be joined on it. Must stay byte-identical to `prefill.cpp`'s loop.
[[nodiscard]] inline std::uint64_t session_key_hash_of(const std::string& session_key) noexcept {
    std::uint64_t owner = 1469598103934665603ULL;
    for (const char byte : session_key) {
        owner ^= static_cast<std::uint8_t>(byte);
        owner *= 1099511628211ULL;
    }
    return owner == 0 ? 1 : owner;
}

[[nodiscard]] inline std::optional<std::string> derive_session_key(std::span<const ChatTurn> messages) {
    const auto text_of = [](const ChatTurn& turn) {
        std::string out;
        for (const ContentPart& part : turn.content) {
            if (part.kind == ContentKind::Text) { out += part.text; }
        }
        return out;
    };
    std::string preimage;
    for (const ChatTurn& turn : messages) {
        if (turn.role == ChatRole::System) {
            preimage += text_of(turn);
            preimage += '\x1f';
        }
    }
    std::string first_user;
    for (const ChatTurn& turn : messages) {
        if (turn.role == ChatRole::User) {
            first_user = text_of(turn);
            break;
        }
    }
    if (first_user.empty()) { return std::nullopt; }
    preimage += first_user;

    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a offset basis, the net's own constant
    for (const unsigned char byte : preimage) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;                 // FNV-1a prime
    }
    char key[32];
    const int written = std::snprintf(key, sizeof(key), "derived-%016llx",
                                      static_cast<unsigned long long>(hash));
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(key)) { return std::nullopt; }
    return std::string(key, static_cast<std::size_t>(written));
}


// Sampling overrides that have an executable Engine meaning. Protocol-only
// fields are normalized or rejected before this value is constructed.
struct SamplingParams {
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<double> min_p;
    std::optional<int> top_k;
    std::optional<double> presence_penalty;
    std::optional<double> frequency_penalty;
    std::optional<std::uint64_t> seed;
};

// Protocol-level effort vocabulary. Each wire adapter accepts the values from
// its external contract; translation passes explicit values to the selected template.
enum class RequestedReasoningEffort : std::uint8_t {
    None,
    Minimal,
    Low,
    Medium,
    High,
    XHigh,
    Max,
};

[[nodiscard]] constexpr std::optional<RequestedReasoningEffort>
parse_requested_reasoning_effort(std::string_view value) noexcept {
    if (value == "none") { return RequestedReasoningEffort::None; }
    if (value == "minimal") { return RequestedReasoningEffort::Minimal; }
    if (value == "low") { return RequestedReasoningEffort::Low; }
    if (value == "medium") { return RequestedReasoningEffort::Medium; }
    if (value == "high") { return RequestedReasoningEffort::High; }
    if (value == "xhigh") { return RequestedReasoningEffort::XHigh; }
    if (value == "max") { return RequestedReasoningEffort::Max; }
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view
requested_reasoning_effort_name(RequestedReasoningEffort effort) noexcept {
    switch (effort) {
    case RequestedReasoningEffort::None:
        return "none";
    case RequestedReasoningEffort::Minimal:
        return "minimal";
    case RequestedReasoningEffort::Low:
        return "low";
    case RequestedReasoningEffort::Medium:
        return "medium";
    case RequestedReasoningEffort::High:
        return "high";
    case RequestedReasoningEffort::XHigh:
        return "xhigh";
    case RequestedReasoningEffort::Max:
        return "max";
    }
    return {};
}

struct GenerationRequest {
    std::vector<ChatTurn> messages;
    std::vector<ToolDefinition> tools;
    std::size_t tool_name_max_length = 64;
    ToolChoice tool_choice;
    std::vector<std::string> stop_strings;
    bool stop_strings_apply_to_reasoning = false;
    int max_tokens                       = 0; // resolved budget; zero means immediate output limit
    std::optional<bool> enable_thinking;      // unset => use the server default
    std::optional<std::uint32_t> thinking_budget;
    std::optional<RequestedReasoningEffort> reasoning_effort;
    std::optional<bool> preserve_thinking;
    std::string chat_template_kwargs_json;
    ninfer::PromptContinuationMode continuation = ninfer::PromptContinuationMode::NewAssistantTurn;
    bool allow_engine_automatic_shared_prefixes = true;
    // HOW MANY EXPLICIT `cache_control` BREAKPOINTS THE CLIENT SENT, counted by the protocol parser with the
    // same code that enforces the cap. Recorded so the request log can carry it: explicit markers become
    // shared-prefix candidates, so without this the 2026-10-01 regression -- where the two candidates were
    // this input and the engine -- had no per-request denominator to separate them by.
    std::size_t explicit_cache_marker_count = 0;
    SamplingParams sampling;

    [[nodiscard]] bool uses_tools() const noexcept {
        return !tools.empty() && tool_choice.mode != ToolChoiceMode::None;
    }

    [[nodiscard]] std::size_t media_item_count() const noexcept {
        std::size_t count = 0;
        for (const ChatTurn& message : messages) {
            for (const ContentPart& part : message.content) {
                if (part.kind == ContentKind::Image || part.kind == ContentKind::Video) { ++count; }
            }
        }
        return count;
    }

    [[nodiscard]] bool has_tool_history() const noexcept {
        for (const ChatTurn& message : messages) {
            if (!message.tool_calls.empty() || message.role == ChatRole::Tool) { return true; }
        }
        return false;
    }
};

} // namespace ninfer::serve
