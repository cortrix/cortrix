#include "cortrix/llm/openai_client.h"

#include <utility>

#include <nlohmann/json.hpp>

#include "cortrix/llm/llm_error_tokens.h"

namespace cortrix::llm {

using json = nlohmann::json;

namespace {

// The OpenAiLlmClient is feature-neutral (shared by 7 consumers), so it does NOT
// know about CX_ERR_ENRICHER_* codes — that classification belongs to the
// F03 LlmEnricher layer. The client reports failures as a plain cortrix::Status:
//   - kUnavailable  → transport failure / HTTP 5xx / HTTP 429 (transient, retryable)
//   - kInternal     → 4xx (non-429) / malformed success body (caller/permanent)
// The message carries a neutral llm_tokens::* token + the http_status so the
// enricher can map it to the right enricher error code + EnricherErrorMeta
// (F03 §4.2 / §5.1).
Status MakeStatus(StatusCode code, const char* token, const std::string& detail) {
    return Status(code, std::string(token) + ": " + detail);
}

}  // namespace

OpenAiLlmClient::OpenAiLlmClient(LlmClientConfig config)
    : config_(std::move(config)), transport_(MakeDefaultHttpTransport()) {}

OpenAiLlmClient::OpenAiLlmClient(LlmClientConfig config,
                                 std::unique_ptr<IHttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {}

ChatCompletionResponse OpenAiLlmClient::Chat(const std::string& prompt,
                                             const LlmCallConfig& call) {
    ChatCompletionResponse resp;

    const std::string model = call.model.empty() ? config_.default_model : call.model;
    const int timeout_ms = call.timeout_ms > 0 ? call.timeout_ms : config_.timeout_ms;
    resp.model = model;

    // --- Build the OpenAI Chat Completions request (S2.1) ---
    json body = {
        {"model", model},
        {"messages", json::array({{{"role", "user"}, {"content", prompt}}})},
        {"temperature", call.temperature},
        {"max_tokens", call.max_tokens},
    };
    if (!call.response_format.empty()) {
        // e.g. {"type":"json_object"} for structured output (topic 3.3 L3 parsing).
        body["response_format"] = {{"type", call.response_format}};
    }

    HttpRequest req;
    req.url = config_.endpoint + "/chat/completions";
    req.body = body.dump();
    req.timeout_ms = timeout_ms;
    req.headers["Content-Type"] = "application/json";
    if (!config_.api_key.empty()) {
        req.headers["Authorization"] = "Bearer " + config_.api_key;
    }

    // --- Single round-trip (retry/backoff/breaker owned by LlmEnricher, §4.2) ---
    HttpResponse http = transport_->Send(req);

    if (!http.network_ok) {
        // DNS / connect / TLS / timeout — no HTTP status reached.
        std::string detail = "transport failure to " + config_.endpoint;
        if (!http.transport_error.empty()) detail += ": " + http.transport_error;
        resp.status = MakeStatus(StatusCode::kUnavailable, llm_tokens::kTransport,
                                 detail);
        return resp;
    }

    // --- HTTP-level error classification (S2.1 / §5.1) ---
    if (http.status_code == 429) {
        std::string ra = http.header("Retry-After");
        // Surface the Retry-After header so the enricher can honor it (§5.1).
        resp.finish_reason = ra;  // carries the header value to the caller
        resp.status = MakeStatus(StatusCode::kUnavailable, llm_tokens::kRateLimit,
                                 ra.empty() ? "429" : ("429 retry-after=" + ra));
        return resp;
    }
    if (http.status_code >= 500) {
        resp.status = MakeStatus(StatusCode::kUnavailable, llm_tokens::kHttp,
                                 "http_status=" + std::to_string(http.status_code));
        return resp;
    }
    if (http.status_code < 200 || http.status_code >= 300) {
        // 4xx (non-429): bad request / auth — permanent from the client's view.
        resp.status = MakeStatus(StatusCode::kInternal, llm_tokens::kHttp,
                                 "http_status=" + std::to_string(http.status_code));
        return resp;
    }

    // --- Parse the success body (no-throw — malformed body = permanent BAD_BODY) ---
    json j = json::parse(http.body, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        resp.status = MakeStatus(StatusCode::kInternal, llm_tokens::kBadBody,
                                 "non-JSON response body");
        return resp;
    }

    // choices[0].message.content + finish_reason
    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
        const json& choice0 = j["choices"][0];
        resp.content = choice0.value("/message/content"_json_pointer, std::string{});
        resp.finish_reason = choice0.value("finish_reason", std::string{});
    }
    // model echoed by server (fallback to requested)
    resp.model = j.value("model", model);
    // usage.{prompt,completion}_tokens (budget input, F03 §3.5)
    if (j.contains("usage") && j["usage"].is_object()) {
        resp.prompt_tokens = j["usage"].value("prompt_tokens", 0);
        resp.completion_tokens = j["usage"].value("completion_tokens", 0);
    }

    resp.status = Status::Ok();
    return resp;
}

}  // namespace cortrix::llm
