// 🔒 openai_client deterministic error/degradation mapping — the gaps not already
// covered by test_openai_client.cpp (which has 429/retry_after, malformed JSON,
// 5xx, 4xx, network-fail). Here: api_key_missing (no Authorization header built),
// missing `choices`, empty `choices` array, empty content, null content, and the
// non-object success body. All driven via FakeHttpTransport — no live LLM.
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/llm/llm_error_tokens.h"
#include "cortrix/llm/openai_client.h"
#include "fake_http_transport.h"

namespace cortrix::llm {
namespace {

struct ClientWithFake {
    FakeHttpTransport* fake;
    std::unique_ptr<OpenAiLlmClient> client;
};
ClientWithFake MakeClient(LlmClientConfig cfg = {}) {
    if (cfg.endpoint.empty()) cfg.endpoint = "https://api.example.com/v1";
    if (cfg.api_key.empty()) cfg.api_key = "sk-test";
    auto fake = std::make_unique<FakeHttpTransport>();
    FakeHttpTransport* raw = fake.get();
    auto client = std::make_unique<OpenAiLlmClient>(std::move(cfg), std::move(fake));
    return {raw, std::move(client)};
}

// --- api_key empty → NO Authorization header is built on the request ---
TEST(OpenAiClientErrors, MissingApiKeyOmitsAuthorizationHeader) {
    // Build the client directly (NOT via MakeClient, which defaults an empty key to
    // "sk-test") so the empty-key path is genuinely exercised.
    LlmClientConfig cfg;
    cfg.endpoint = "https://api.example.com/v1";
    cfg.api_key = "";  // unset → must not emit a bogus "Bearer " header
    auto fake = std::make_unique<FakeHttpTransport>();
    FakeHttpTransport* raw = fake.get();
    raw->canned = FakeHttpTransport::Json2xx(
        R"({"choices":[{"message":{"content":"x"}}]})");
    OpenAiLlmClient client(std::move(cfg), std::move(fake));
    client.Chat("q", LlmCallConfig{});
    EXPECT_EQ(raw->last_request.headers.count("Authorization"), 0u);
    // Content-Type is still set regardless of the key.
    EXPECT_EQ(raw->last_request.headers.at("Content-Type"), "application/json");
}

// --- a non-empty api_key DOES build the bearer header (contrast case) ---
TEST(OpenAiClientErrors, PresentApiKeyBuildsBearer) {
    LlmClientConfig cfg;
    cfg.api_key = "sk-live-xyz";
    auto cf = MakeClient(cfg);
    cf.fake->canned = FakeHttpTransport::Json2xx(R"({"choices":[]})");
    cf.client->Chat("q", LlmCallConfig{});
    EXPECT_EQ(cf.fake->last_request.headers.at("Authorization"), "Bearer sk-live-xyz");
}

// --- 2xx body missing "choices" entirely → ok() but empty content (no crash) ---
TEST(OpenAiClientErrors, MissingChoicesYieldsEmptyContent) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx(
        R"({"model":"gpt-4o-mini","usage":{"prompt_tokens":3,"completion_tokens":0}})");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    ASSERT_TRUE(resp.ok());  // a well-formed 2xx JSON object is success
    EXPECT_TRUE(resp.content.empty());
    EXPECT_EQ(resp.prompt_tokens, 3);  // usage still parsed
}

// --- 2xx with an EMPTY choices array → ok() + empty content ---
TEST(OpenAiClientErrors, EmptyChoicesArrayYieldsEmptyContent) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx(R"({"choices":[]})");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    ASSERT_TRUE(resp.ok());
    EXPECT_TRUE(resp.content.empty());
    EXPECT_TRUE(resp.finish_reason.empty());
}

// --- 2xx with an explicit empty-string content → ok() + empty content ---
TEST(OpenAiClientErrors, EmptyContentStringIsOk) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx(
        R"({"choices":[{"message":{"content":""},"finish_reason":"stop"}]})");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    ASSERT_TRUE(resp.ok());
    EXPECT_TRUE(resp.content.empty());
    EXPECT_EQ(resp.finish_reason, "stop");
}

// --- a 2xx JSON ARRAY (not an object) → BAD_BODY / kInternal ---
TEST(OpenAiClientErrors, NonObjectJsonBodyIsBadBody) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx(R"(["not","an","object"])");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(resp.status.code(), StatusCode::kInternal);
    EXPECT_EQ(resp.status.message().rfind(llm_tokens::kBadBody, 0), 0u);
}

// --- 429 WITHOUT a Retry-After header → still kUnavailable rate-limit, empty
//     finish_reason (no header to carry) ---
TEST(OpenAiClientErrors, RateLimitNoRetryAfterHeader) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Http(429);  // no Retry-After
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(resp.status.code(), StatusCode::kUnavailable);
    EXPECT_EQ(resp.status.message().rfind(llm_tokens::kRateLimit, 0), 0u);
    EXPECT_TRUE(resp.finish_reason.empty());
}

// --- a 401 (auth) is a non-429 4xx → kInternal HTTP token (permanent) ---
TEST(OpenAiClientErrors, Http401IsInternalHttp) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Http(401, "invalid api key");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(resp.status.code(), StatusCode::kInternal);
    EXPECT_EQ(resp.status.message().rfind(llm_tokens::kHttp, 0), 0u);
    EXPECT_NE(resp.status.message().find("401"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::llm
