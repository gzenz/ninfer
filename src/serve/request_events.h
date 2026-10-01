#pragma once

#include "serve/generation_service.h"
#include "serve/request.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ninfer::serve {

struct RequestLogContext {
    std::uint64_t id = 0;
    std::string protocol;
    std::string model;
    bool stream                             = false;
    std::size_t message_count               = 0;
    std::size_t media_item_count            = 0;
    int requested_output_tokens             = 0;
    bool requested_output_tokens_client_set = false;
    std::size_t tool_count                  = 0;
    ToolChoice tool_choice;
    bool has_tool_history = false;
    bool enable_thinking  = true;
    std::optional<std::uint32_t> thinking_budget;
    std::optional<RequestedReasoningEffort> requested_reasoning_effort;
    std::optional<bool> preserve_thinking;
    bool preserve_thinking_semantic_change = false;
    ninfer::ResolvedSamplingParameters sampling;
    double acquisition_seconds = 0.0;
    ninfer::PromptPreparationStats preparation;
    std::optional<std::string> session_key;
    std::optional<std::string> client_session_id;
    std::optional<std::string> session_key_hash;
    // HOW MANY EXPLICIT `cache_control` BREAKPOINTS THE REQUEST CARRIED. This is the denominator for the
    // cost question the 2026-10-01 regression raised: explicit markers become shared-prefix candidates, so
    // "is the planner's work proportional to the markers?" cannot be asked of any log without it -- and the
    // two candidates for that regression (this input vs the engine) are indistinguishable per request
    // without it. Counted on the request, not on the prepared prompt, so it describes what the CLIENT sent.
    // ANTHROPIC PATH ONLY (as of 2026-10-01). The OpenAI translators synthesise a MIX of explicit and
    // automatic markers and do not separate them, so that path logs 0 -- which means "not counted
    // here", NOT "the client sent none". Read 0 as absent, never as zero.
    std::size_t explicit_cache_markers = 0;
};

struct RequestLogMetadata {
    std::string model;
    bool stream                            = false;
    bool output_tokens_explicit            = false;
    bool preserve_thinking_semantic_change = false;
    // THE DERIVED SESSION KEY, so key stability is checkable from traffic instead of inferred. Without it in
    // the record, "does one conversation keep one key" and "do two concurrent streams share one key" are
    // questions no log can answer -- and both are live: the port derives the key from system text plus the
    // FIRST user turn, which an agentic client's sibling requests share. The value is an FNV-1a digest of that
    // text, not the text.
    std::optional<std::string> session_key;
    // The client's own session id, hashed, when the body carries `metadata.user_id`. Logged beside the
    // derived key so the two are comparable per request: if the client sends a stable id, the derived FNV
    // preimage -- which a client's concurrent siblings SHARE -- can be replaced by it at the root.
    std::optional<std::string> client_session_id;
    // THE SAME HASH `prefill.cpp` puts on the sequence and the eviction line prints as `session=%016llx`, so an
    // eviction can be tied to the request whose state it destroyed. Rendered as 16 hex digits for that match.
    std::optional<std::string> session_key_hash;
    // HOW MANY EXPLICIT `cache_control` BREAKPOINTS THE REQUEST CARRIED. This is the denominator for the
    // cost question the 2026-10-01 regression raised: explicit markers become shared-prefix candidates, so
    // "is the planner's work proportional to the markers?" cannot be asked of any log without it -- and the
    // two candidates for that regression (this input vs the engine) are indistinguishable per request
    // without it. Counted on the request, not on the prepared prompt, so it describes what the CLIENT sent.
    std::size_t explicit_cache_markers = 0;
};

// A parsed generation request that failed during synchronous preparation. It intentionally has a
// separate shape because sampler and prompt semantics may not have resolved.
struct RequestRejectionLogContext {
    std::uint64_t id = 0;
    std::string protocol;
    std::string model;
    bool stream                             = false;
    std::size_t message_count               = 0;
    std::size_t media_item_count            = 0;
    int requested_output_tokens             = 0;
    bool requested_output_tokens_client_set = false;
    std::size_t tool_count                  = 0;
    ToolChoice tool_choice;
    bool has_tool_history = false;
    std::optional<RequestedReasoningEffort> requested_reasoning_effort;
    ApiError error;
    // DID THE BODY PARSE? False on the pre-parse rejection path, where every other field in this context
    // is a DEFAULT rather than a measurement -- and a reader must be able to tell those apart, which a
    // record of zeroed fields does not do. The emitter renders `parsed` and a `phase` derived from it.
    bool parsed = true;
};

enum class RequestFailurePhase : std::uint8_t {
    Prepare,
    Generation,
    ResponseRender,
    ResponseStore,
    Transport,
    Http,
};

enum class RequestFailureClass : std::uint8_t {
    ClientInput,
    ClientDisconnected,
    Overload,
    Timeout,
    Unavailable,
    Upstream,
    Internal,
};

struct RequestFailure {
    RequestFailurePhase phase          = RequestFailurePhase::Generation;
    RequestFailureClass classification = RequestFailureClass::Internal;
    int http_status                    = 0;
    std::string error_type;
    std::string error_code;
    std::string param;
    // Used only by the independent JSONL measurement writer. Operational rendering never consumes
    // this field.
    std::string machine_message;
};

struct ThroughputReport {
    double interval_seconds               = 0.0;
    std::uint64_t computed_prefill_tokens = 0;
    std::uint64_t committed_decode_tokens = 0;
    std::uint64_t decode_rounds           = 0;
    std::uint64_t decode_row_rounds       = 0;
    ninfer::RuntimeStats previous;
    ninfer::RuntimeStats current;
};

RequestLogContext make_request_log_context(std::uint64_t id, std::string protocol,
                                           const GenerationRequest& request,
                                           const RequestLogMetadata& metadata,
                                           const PreparedRequest& prepared);
RequestRejectionLogContext make_request_rejection_log_context(std::uint64_t id,
                                                              std::string protocol,
                                                              const GenerationRequest& request,
                                                              const RequestLogMetadata& metadata,
                                                              ApiError error);

// A REJECTION THAT HAPPENS BEFORE THE BODY PARSES. The overload above needs a parsed request and its
// metadata, so the parse-time catch could not use it -- and nothing was logged at all, which is how a
// client sending five cache_control breakpoints saw a 400 from a server whose journal and request log
// were both empty (measured 2026-10-01). Everything the parsed overload reads is genuinely unknown here,
// so only the identity, the protocol and the error are carried, and the reader is told that by the record
// itself rather than by an absence of fields that looks the same as a request that had none.
[[nodiscard]] RequestRejectionLogContext make_unparsed_request_rejection_log_context(std::uint64_t id,
                                                                                    std::string protocol,
                                                                                    ApiError error);

[[nodiscard]] RequestFailure make_request_failure(RequestFailurePhase phase, const ApiError& error);
[[nodiscard]] RequestFailure make_generation_request_failure(const ApiError& error);
[[nodiscard]] RequestFailure make_internal_request_failure(RequestFailurePhase phase,
                                                           std::string machine_message);
[[nodiscard]] RequestFailure make_client_disconnected_failure(RequestFailurePhase phase);

} // namespace ninfer::serve
