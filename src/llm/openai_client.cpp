#include "cortrix/llm/openai_client.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

#include "cortrix/llm/llm_error_tokens.h"

namespace cortrix::llm {

using json = nlohmann::json;

namespace {

// The OpenAiLlmClient is feature-neutral (shared by 7 consumers), so it does NOT
// know about CX_ERR_ENRICHER_* codes — that classification belongs to the
// LlmEnricher layer. The client reports failures as a plain cortrix::Status:
//   - kUnavailable  → transport failure / HTTP 5xx / HTTP 429 (transient, retryable)
//   - kInternal     → 4xx (non-429) / malformed success body (caller/permanent)
// The message carries a neutral llm_tokens::* token + the http_status so the
// enricher can map it to the right enricher error code + EnricherErrorMeta
// (per the enricher contract).
Status MakeStatus(StatusCode code, const char* token, const std::string& detail) {
    return Status(code, std::string(token) + ": " + detail);
}

std::string ReadStringField(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return std::string{};
    return it->get<std::string>();
}

int ReadIntField(const json& obj, const char* key, int fallback = 0) {
    auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_number_unsigned()) {
        const auto v = it->get<unsigned long long>();
        if (v > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
            return fallback;
        }
        return static_cast<int>(v);
    }
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        char* end = nullptr;
        errno = 0;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (errno == 0 && end != s.c_str() && *end == '\0' &&
            v >= std::numeric_limits<int>::min() &&
            v <= std::numeric_limits<int>::max()) {
            return static_cast<int>(v);
        }
    }
    return fallback;
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
    if (!call.thinking_type.empty()) {
        // DeepSeek-compatible providers expose thinking control through this
        // request object. Leave it absent by default for broad compatibility.
        body["thinking"] = {{"type", call.thinking_type}};
    }

    HttpRequest req;
    req.url = config_.endpoint + "/chat/completions";
    req.body = body.dump();
    req.timeout_ms = timeout_ms;
    req.headers["Content-Type"] = "application/json";
    if (!config_.api_key.empty()) {
        req.headers["Authorization"] = "Bearer " + config_.api_key;
    }

    // --- Bounded transport retry ---
    // HTTP-level retry/backoff remains feature-owned. At the shared client layer,
    // retry only no-status transport failures because consumers such as doc-summary do not
    // have an outer retry shell.
    HttpResponse http;
    const int attempts = std::max(1, config_.max_retries + 1);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        http = transport_->Send(req);
        if (http.network_ok) break;
    }

    if (!http.network_ok) {
        // DNS / connect / TLS / timeout — no HTTP status reached.
        std::string detail = "transport failure to " + config_.endpoint;
        if (!http.transport_error.empty()) detail += ": " + http.transport_error;
        resp.status = MakeStatus(StatusCode::kUnavailable, llm_tokens::kTransport,
                                 detail);
        return resp;
    }

    // --- HTTP-level error classification (S2.1 /) ---
    if (http.status_code == 429) {
        std::string ra = http.header("Retry-After");
        // Surface the Retry-After header so the enricher can honor it.
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

    // choices[0].message.content + finish_reason. Some OpenAI-compatible
    // providers return the final structured answer in message.reasoning_content
    // when message.content is empty. Use that as a compatibility fallback while
    // exposing only field names and lengths for observability.
    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
        const json& choice0 = j["choices"][0];
        resp.finish_reason = ReadStringField(choice0, "finish_reason");
        auto mit = choice0.find("message");
        if (mit != choice0.end() && mit->is_object()) {
            const std::string message_content = ReadStringField(*mit, "content");
            const std::string reasoning_content = ReadStringField(*mit, "reasoning_content");
            resp.reasoning_content_length = static_cast<int>(reasoning_content.size());
            if (!message_content.empty() || reasoning_content.empty() ||
                !call.allow_reasoning_content_fallback) {
                resp.content = message_content;
                resp.content_source = "message.content";
            } else {
                resp.content = reasoning_content;
                resp.content_source = "message.reasoning_content";
            }
            resp.content_length = static_cast<int>(resp.content.size());
        }
    }
    // model echoed by server (fallback to requested)
    resp.model = j.value("model", model);
    // usage.{prompt,completion}_tokens (budget input)
    if (j.contains("usage") && j["usage"].is_object()) {
        resp.prompt_tokens = ReadIntField(j["usage"], "prompt_tokens");
        resp.completion_tokens = ReadIntField(j["usage"], "completion_tokens");
    }

    resp.status = Status::Ok();
    return resp;
}

}  // namespace cortrix::llm
