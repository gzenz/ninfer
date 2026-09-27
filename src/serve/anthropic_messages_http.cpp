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

    AnthropicMessagesRequest request;
    try {
        RequestLimits limits;
        limits.default_max_tokens = options_.default_max_tokens;
        request                   = parse_anthropic_messages_request(parse_json_body(req), limits);
    } catch (const ApiException& exception) {
        write_anthropic_error(res, exception.error(), request_id);
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

    const std::uint64_t req_id = ++request_seq_;
    // DERIVED BEFORE THE METADATA IS BUILT, so the key the engine is handed and the key the log records cannot
    // disagree. THE V2 SESSION-KEY PORT (613171bd): the Messages path carries no session key, so derive one
    // from the conversation's own first turn. Without it every request landed in its own reuse domain
    // (`reuse_domain` falls back to the per-request publication_order) and retention stayed RecentPrivate,
    // which is the condition V2 describes as units that "classify dead forever".
    ContextCacheHints cache_hints;
    cache_hints.session_key = derive_session_key(request.generation.messages);
    const RequestLogMetadata metadata{.model                  = request.model,
                                      .stream                 = request.stream,
                                      .output_tokens_explicit = request.output_tokens_explicit,
                                      .session_key            = cache_hints.session_key};
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
