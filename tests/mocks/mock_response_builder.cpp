#include "mock_response_builder.h"

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/llm/llm_error_tokens.h"

namespace cortrix::llm {

namespace {
using json = nlohmann::json;

// One chunk's enrichment object (the enricher prompt's expected per-index shape).
json ChunkObj(int i) {
    return {
        {"entities", json::array({{{"text", "Entity" + std::to_string(i)},
                                   {"type", "ORG"},
                                   {"start_offset", 0},
                                   {"end_offset", 6}}})},
        {"summary", "summary of chunk " + std::to_string(i)},
        {"score", 0.5},
    };
}

ChatCompletionResponse OkWith(std::string content, const std::string& model,
                              int pt = 100, int ct = 50) {
    ChatCompletionResponse r;
    r.status = Status::Ok();
    r.content = std::move(content);
    r.finish_reason = "stop";
    r.model = model;
    r.prompt_tokens = pt;
    r.completion_tokens = ct;
    return r;
}
}  // namespace

ChatCompletionResponse MockResponseBuilder::Normal(int batch_size, const std::string& model) {
    json obj = json::object();
    for (int i = 0; i < batch_size; ++i) obj[std::to_string(i)] = ChunkObj(i);
    return OkWith(obj.dump(), model);
}

ChatCompletionResponse MockResponseBuilder::Empty(const std::string& model) {
    return OkWith("", model, /*pt=*/0, /*ct=*/0);
}

ChatCompletionResponse MockResponseBuilder::MalformedJson(const std::string& model) {
    return OkWith("}{ this is not valid json", model);
}

ChatCompletionResponse MockResponseBuilder::PartialBatch(int returned, int expected,
                                                         const std::string& model) {
    json obj = json::object();
    for (int i = 0; i < returned && i < expected; ++i) obj[std::to_string(i)] = ChunkObj(i);
    return OkWith(obj.dump(), model);
}

ChatCompletionResponse MockResponseBuilder::Timeout(const std::string& model) {
    ChatCompletionResponse r;
    r.model = model;
    r.status = Status::Unavailable(std::string(llm_tokens::kTransport) + ": timeout");
    return r;
}

ChatCompletionResponse MockResponseBuilder::HttpError(int status_code, const std::string& model) {
    ChatCompletionResponse r;
    r.model = model;
    // 5xx → Unavailable (retryable); 4xx → Internal (permanent). Same mapping the
    // real OpenAiLlmClient uses, so the enricher classifies identically.
    StatusCode code = status_code >= 500 ? StatusCode::kUnavailable : StatusCode::kInternal;
    r.status = Status(code, std::string(llm_tokens::kHttp) + ": http_status=" +
                                std::to_string(status_code));
    return r;
}

ChatCompletionResponse MockResponseBuilder::RateLimit(int retry_after_sec, const std::string& model) {
    ChatCompletionResponse r;
    r.model = model;
    r.finish_reason = std::to_string(retry_after_sec);  // Retry-After carried out
    r.status = Status::Unavailable(std::string(llm_tokens::kRateLimit) + ": 429 retry-after=" +
                                   std::to_string(retry_after_sec));
    return r;
}

ChatCompletionResponse MockResponseBuilder::BudgetExceeded(const std::string& model) {
    // A valid single-chunk OK response with very large usage so accumulating it
    // crosses any modest budget cap (the enricher's BudgetTracker gate fires).
    json obj = {{"0", ChunkObj(0)}};
    return OkWith(obj.dump(), model, /*pt=*/20'000'000, /*ct=*/0);
}

}  // namespace cortrix::llm
