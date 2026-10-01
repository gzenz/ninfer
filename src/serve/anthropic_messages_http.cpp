#include "serve/http_server.h"

#include "serve/anthropic_messages.h"
#include "serve/http_transport.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ninfer::serve {

void HttpServer::handle_count_tokens(const httplib::Request& req, httplib::Response& res) {
    const std::string request_id = new_anthropic_request_id();
    res.set_header("request-id", request_id);
    try {
        const AnthropicCountTokensRequest request =
            parse_anthropic_count_tokens_request(parse_json_body(req));
        const int input_tokens = service_->count_prompt_tokens(
            request.generation, [&req] { return client_disconnected(req); });
        res.set_content(make_anthropic_count_tokens_response(input_tokens), "application/json");
    } catch (const ApiException& exception) {
        write_anthropic_error(res, exception.error(), request_id);
    } catch (const std::exception& exception) {
        operational_log_.http_failure(
            "anthropic_count_tokens",
            make_internal_request_failure(RequestFailurePhase::Http, exception.what()), request_id);
        ApiError error;
        error.status  = 500;
        error.message = exception.what();
        write_anthropic_error(res, error, request_id);
    }
}

void HttpServer::handle_messages(const httplib::Request& req, httplib::Response& res) {
    const std::string request_id = new_anthropic_request_id();
    res.set_header("request-id", request_id);

    // THE ID IS TAKEN BEFORE THE PARSE, so a request rejected ON THE WAY IN still gets one. It used to be
    // taken after, which is why the parse-time path below could log nothing: no id existed to log under.
    const std::uint64_t req_id = ++request_seq_;

    AnthropicMessagesRequest request;
    try {
        RequestLimits limits;
        limits.default_max_tokens = options_.default_max_tokens;
        request                   = parse_anthropic_messages_request(parse_json_body(req), limits);
    } catch (const ApiException& exception) {
        // LOGGED, WHICH IT WAS NOT BEFORE. This branch wrote the error to the client and returned, so a
        // rejected request left NOTHING behind -- no `request_rejected` event and no journal line, while
        // the generic branch below did log. Measured 2026-10-01: a client exceeding the cache_control
        // breakpoint cap saw a 400 from a server whose journal and request log both looked idle and
        // healthy, and the failure had to be reproduced by hand to be identified.
        const ApiError error = normalize_anthropic_error(exception.error());
        record_request_rejected(
            make_unparsed_request_rejection_log_context(req_id, "anthropic_messages", error));
        // `make_request_failure`, NOT `make_internal_request_failure`: the latter renders a fixed
        // `HTTP 500 | internal error` regardless of what happened, so the first version of this line
        // reported a 400 cache_control rejection as an internal 500 -- a false status in the journal of
        // the change whose whole purpose was to make rejections legible.
        operational_log_.http_failure(
            "anthropic_messages", make_request_failure(RequestFailurePhase::Http, error), request_id);
        write_anthropic_error(res, error, request_id);
        return;
    } catch (const std::exception& exception) {
        operational_log_.http_failure(
            "anthropic_messages",
            make_internal_request_failure(RequestFailurePhase::Http, exception.what()), request_id);
        ApiError error;
        error.status  = 500;
        error.message = exception.what();
        write_anthropic_error(res, error, request_id);
        return;
    }

    // DERIVED BEFORE THE METADATA IS BUILT, so the key the engine is handed and the key the log records cannot
    // disagree. THE V2 SESSION-KEY PORT (613171bd): the Messages path carries no session key, so derive one
    // from the conversation's own first turn. Without it every request landed in its own reuse domain
    // (`reuse_domain` falls back to the per-request publication_order) and retention stayed RecentPrivate,
    // which is the condition V2 describes as units that "classify dead forever".
    ContextCacheHints cache_hints;
    cache_hints.session_key = derive_session_key(request.generation.messages);
    RequestLogMetadata metadata{.model                  = request.model,
                                .stream                 = request.stream,
                                .output_tokens_explicit = request.output_tokens_explicit,
                                .session_key            = cache_hints.session_key,
                                .client_session_id      = request.metadata_session_hash};
    // Set AFTER the designated initializer, because the field is declared last and C++ requires the
    // designators to follow declaration order.
    metadata.explicit_cache_markers = request.generation.explicit_cache_marker_count;
    // The join key with the engine's eviction line (`session=%016llx`): the SAME hash over the SAME bytes that
    // `prefill.cpp` puts on the sequence, so an eviction can be matched to the request whose state it destroyed.
    if (cache_hints.session_key) {
        char hex[24];
        std::snprintf(hex, sizeof(hex), "%016llx",
                      static_cast<unsigned long long>(session_key_hash_of(*cache_hints.session_key)));
        metadata.session_key_hash = std::string(hex);
    }
    PreparedRequest prepared;
    try {
        prepared = service_->prepare(request.generation,
                                     request.stream ? GenerationConsumerMode::Streaming
                                                    : GenerationConsumerMode::Aggregate,
                                     {}, [&req] { return client_disconnected(req); }, cache_hints);
    } catch (const ApiException& exception) {
        const ApiError error = normalize_anthropic_error(exception.error());
        record_request_rejected(make_request_rejection_log_context(
            req_id, "anthropic_messages", request.generation, metadata, error));
        write_anthropic_error(res, error, request_id);
        return;
    } catch (const std::exception& exception) {
        ApiError error;
        error.status  = 500;
        error.type    = "internal_error";
        error.message = exception.what();
        record_request_rejected(make_request_rejection_log_context(
            req_id, "anthropic_messages", request.generation, metadata, error));
        write_anthropic_error(res, error, request_id);
        return;
    }

    const AnthropicResponseIdentity identity =
        make_anthropic_response_identity(request_id, request.model);
    const int input_tokens = prepared.prompt_tokens;

    auto lifecycle = begin_request(make_request_log_context(
        req_id, "anthropic_messages", request.generation, metadata, prepared));

    if (!request.stream) {
        GenerationOutcome outcome;
        try {
            outcome = service_->run(prepared, nullptr, [&req] { return client_disconnected(req); });
        } catch (const ApiException& exception) {
            const ApiError error = normalize_anthropic_error(exception.error());
            lifecycle->failure(make_generation_request_failure(error));
            write_anthropic_error(res, error, request_id);
            return;
        } catch (const std::exception& exception) {
            lifecycle->failure(
                make_internal_request_failure(RequestFailurePhase::Generation, exception.what()));
            ApiError error;
            error.status  = 500;
            error.message = exception.what();
            write_anthropic_error(res, error, request_id);
            return;
        }
        lifecycle->done(outcome);
        // Response provenance (NINFER_RESP_PROBE=1). Every engine-internal per-lane binding is
        // verified clean, so the remaining question is whether a foreign reply is *produced* for
        // this prompt or *delivered* to the wrong client. Printing the canary found in the
        // request text next to the one found in the reply settles it: prompt=S1/reply=S0 is
        // content corruption; prompt=S1/reply=S1 while the client received S0's text is routing.
        if (std::getenv("NINFER_RESP_PROBE") != nullptr) {
            const auto canary_of = [](const std::string& text) -> std::string {
                const std::size_t at = text.find("CANARY-");
                if (at == std::string::npos) { return {}; }
                return text.substr(at, std::min<std::size_t>(text.size() - at, 20));
            };
            std::string prompt_text;
            for (const ChatTurn& turn : request.generation.messages) {
                for (const ContentPart& part : turn.content) { prompt_text += part.text; }
            }
            const std::string reply_text = outcome.text + outcome.reasoning;
            std::fprintf(stderr,
                         "[resp-probe] rid=%s prompt_tokens=%d completion=%d prompt_canary=%s "
                         "reply_canary=%s reply_bytes=%zu\n",
                         request_id.c_str(), input_tokens, outcome.completion_tokens,
                         canary_of(prompt_text).c_str(), canary_of(reply_text).c_str(),
                         reply_text.size());
            std::fflush(stderr);
        }
        try {
            set_owned_json_content(res, make_anthropic_messages_response(identity, outcome),
                                   prepared.lifetime);
        } catch (const ApiException& exception) {
            const ApiError error = normalize_anthropic_error(exception.error());
            lifecycle->response_failure(
                make_request_failure(RequestFailurePhase::ResponseRender, error));
            write_anthropic_error(res, error, request_id);
        } catch (const std::exception& exception) {
            lifecycle->response_failure(make_internal_request_failure(
                RequestFailurePhase::ResponseRender, exception.what()));
            ApiError error;
            error.status  = 500;
            error.message = exception.what();
            write_anthropic_error(res, error, request_id);
        }
        return;
    }

    try {
        auto stream  = std::make_shared<HttpGenerationStream>(std::move(prepared));
        auto encoder = std::make_shared<AnthropicMessagesStream>(identity, input_tokens);

        prepare_sse_response(res);
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, stream, encoder, lifecycle](std::size_t, httplib::DataSink& sink) -> bool {
                if (stream->started.exchange(true, std::memory_order_acq_rel)) {
                    sink.done();
                    return true;
                }
                SseTransport transport(sink, stream->cancelled);
                const auto send_error = [&](const ApiError& error) {
                    try {
                        if (!encoder->started()) {
                            render_and_write(transport, [&] { return encoder->start(); });
                        }
                        render_and_write(transport, [&] { return encoder->error(error); });
                        sink.done();
                        return true;
                    } catch (const ClientDisconnected&) {
                        lifecycle->response_failure(
                            make_client_disconnected_failure(RequestFailurePhase::Transport));
                        return false;
                    } catch (const ResponseRenderFailure& exception) {
                        lifecycle->response_failure(make_internal_request_failure(
                            RequestFailurePhase::ResponseRender, exception.what()));
                        return false;
                    }
                };

                GenerationOutcome outcome;
                try {
                    StreamSink output;
                    output.on_start = [&](const ninfer::GenerationStart& start) {
                        render_and_write(transport, [&] { return encoder->start(start); });
                    };
                    output.on_reasoning = [&](const std::string& text) {
                        render_and_write(transport, [&] { return encoder->reasoning_delta(text); });
                    };
                    output.on_content = [&](const std::string& text) {
                        render_and_write(transport, [&] { return encoder->content_delta(text); });
                    };
                    output.is_cancelled = [&] { return transport.poll(); };

                    outcome = service_->run(stream->prepared, &output);
                } catch (const ClientDisconnected&) {
                    lifecycle->failure(
                        make_client_disconnected_failure(RequestFailurePhase::Transport));
                    return false;
                } catch (const ResponseRenderFailure& exception) {
                    lifecycle->failure(make_internal_request_failure(
                        RequestFailurePhase::ResponseRender, exception.what()));
                    ApiError error;
                    error.status  = 500;
                    error.message = exception.what();
                    return send_error(error);
                } catch (const ApiException& exception) {
                    const ApiError error = normalize_anthropic_error(exception.error());
                    lifecycle->failure(make_generation_request_failure(error));
                    return send_error(error);
                } catch (const std::exception& exception) {
                    lifecycle->failure(make_internal_request_failure(
                        RequestFailurePhase::Generation, exception.what()));
                    ApiError error;
                    error.status  = 500;
                    error.message = exception.what();
                    return send_error(error);
                }

                lifecycle->done(outcome);
                std::vector<std::string> terminal;
                try {
                    terminal = encoder->finish(outcome);
                } catch (const std::exception& exception) {
                    // A CLIENT THAT WENT AWAY IS NOT AN INTERNAL ERROR. Classifying this as one put a false
                    // `ERROR ... HTTP 500 | internal error` in the journal for every such request, and it is
                    // what tripped the soak's own "HTTP 500 burst" check: measured 2026-10-01, 13
                    // render-failures in one load, ALL 13 on cancelled requests, all 13 paired with a 499 on
                    // the same id -- i.e. every 500 in the load was this.
                    //
                    // THE MECHANISM, CORRECTED: the first version of this comment said the render "fails
                    // because the socket is gone". It does not -- `AnthropicMessagesStream::finish` never
                    // touches the socket. The 13 cases were cancelled while still QUEUED (`output 0`), which
                    // is the `!started_` guard throwing "invalid Anthropic stream finish state". So
                    // `cancelled` is a sound proxy for "the client is gone" (it is set only by the transport
                    // poll/write failure and the completion callback) but NOT a sound proxy for "this throw
                    // was CAUSED by the client leaving".
                    //
                    // WHAT HAPPENS TO THE TEXT, STATED SO IT IS NOT ASSUMED. It is put in `machine_message`,
                    // whose documented destination is the JSONL measurement writer -- and this path writes no
                    // JSONL event, while the operational log deliberately refuses to render that field (a
                    // tested data policy: `test_request_log.cpp` requires the detail to be ABSENT from the
                    // rendered line). So on this path the text is stored and NOT currently visible anywhere.
                    // The masking is reduced rather than removed: a genuine error landing behind a 499 still
                    // reads as a 499, and making it readable needs a sink this change did not add.
                    if (stream->cancelled.load(std::memory_order_acquire)) {
                        RequestFailure cancelled_failure =
                            make_client_disconnected_failure(RequestFailurePhase::Transport);
                        cancelled_failure.machine_message = exception.what();
                        lifecycle->response_failure(cancelled_failure);
                        return false;
                    }
                    lifecycle->response_failure(make_internal_request_failure(
                        RequestFailurePhase::ResponseRender, exception.what()));
                    ApiError error;
                    error.status  = 500;
                    error.message = exception.what();
                    return send_error(error);
                }
                try {
                    transport.write(terminal);
                    sink.done();
                    return true;
                } catch (const ClientDisconnected&) {
                    lifecycle->response_failure(
                        make_client_disconnected_failure(RequestFailurePhase::Transport));
                    return false;
                }
            },
            [stream, lifecycle](bool successful) {
                stream->cancelled.store(true, std::memory_order_release);
                if (!successful || !stream->started.load(std::memory_order_acquire)) {
                    lifecycle->failure(
                        make_client_disconnected_failure(RequestFailurePhase::Transport));
                }
            });
    } catch (const std::exception& exception) {
        lifecycle->failure(
            make_internal_request_failure(RequestFailurePhase::ResponseRender, exception.what()));
        ApiError error;
        error.status  = 500;
        error.message = exception.what();
        write_anthropic_error(res, error, request_id);
    }
}

} // namespace ninfer::serve
