#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/llm/llm_error_tokens.h"
#include "cortrix/llm/openai_client.h"
#include "fake_http_transport.h"
#include "mock_llm_client.h"

namespace cortrix::llm {
namespace {

using ::testing::_;
using ::testing::Return;

TEST(LlmConfigTest, Defaults) {
    LlmCallConfig call;
    EXPECT_EQ(call.temperature, 0.0);
    EXPECT_EQ(call.max_tokens, 1024);
    EXPECT_EQ(call.thinking_type, "");

    LlmClientConfig client;
    EXPECT_EQ(client.default_model, "gpt-4o-mini");
    EXPECT_EQ(client.max_retries, 2);
    EXPECT_EQ(client.circuit_breaker_cooldown_ms, 60000);
}

TEST(ChatCompletionResponseTest, OkTracksStatus) {
    ChatCompletionResponse good;
    good.content = "hello";
    EXPECT_TRUE(good.ok());  // default Status is Ok

    ChatCompletionResponse bad;
    bad.status = Status::Unavailable("rate limited");
    EXPECT_FALSE(bad.ok());
}

// Helper: client wired to a FakeHttpTransport we keep a borrowed pointer to.
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

TEST(OpenAiLlmClientTest, BuildsOpenAiRequestShape) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx(
        R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}],
            "model":"gpt-4o-mini","usage":{"prompt_tokens":5,"completion_tokens":3}})");

    LlmCallConfig call;
    call.model = "gpt-4o-mini";
    call.response_format = "json_object";
    call.thinking_type = "disabled";
    auto resp = cf.client->Chat("summarize this", call);

    ASSERT_TRUE(resp.ok());
    // Request shape: URL, Authorization, JSON body.
    EXPECT_EQ(cf.fake->last_request.url, "https://api.example.com/v1/chat/completions");
    EXPECT_EQ(cf.fake->last_request.headers.at("Authorization"), "Bearer sk-test");
    EXPECT_EQ(cf.fake->last_request.headers.at("Content-Type"), "application/json");
    auto body = nlohmann::json::parse(cf.fake->last_request.body);
    EXPECT_EQ(body["model"], "gpt-4o-mini");
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"], "summarize this");
    EXPECT_EQ(body["response_format"]["type"], "json_object");
    EXPECT_EQ(body["thinking"]["type"], "disabled");
}

TEST(OpenAiLlmClientTest, ParsesContentAndUsage) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx(
        R"({"choices":[{"message":{"content":"the answer"},"finish_reason":"stop"}],
            "model":"gpt-4o-mini","usage":{"prompt_tokens":11,"completion_tokens":7}})");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    ASSERT_TRUE(resp.ok());
    EXPECT_EQ(resp.content, "the answer");
    EXPECT_EQ(resp.finish_reason, "stop");
    EXPECT_EQ(resp.model, "gpt-4o-mini");
    EXPECT_EQ(resp.prompt_tokens, 11);
    EXPECT_EQ(resp.completion_tokens, 7);
    EXPECT_EQ(resp.content_source, "message.content");
    EXPECT_EQ(resp.content_length, 10);
    EXPECT_EQ(resp.reasoning_content_length, 0);
}

TEST(OpenAiLlmClientTest, FallsBackToReasoningContentWhenContentEmpty) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx(
        R"({"choices":[{"message":{"content":"","reasoning_content":"{\"ok\":true}"},"finish_reason":"length"}],
            "model":"glm-5.2","usage":{"prompt_tokens":"11","completion_tokens":"7"}})");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    ASSERT_TRUE(resp.ok());
    EXPECT_EQ(resp.content, R"({"ok":true})");
    EXPECT_EQ(resp.content_source, "message.reasoning_content");
    EXPECT_EQ(resp.finish_reason, "length");
    EXPECT_EQ(resp.model, "glm-5.2");
    EXPECT_EQ(resp.prompt_tokens, 11);
    EXPECT_EQ(resp.completion_tokens, 7);
    EXPECT_EQ(resp.content_length, 11);
    EXPECT_EQ(resp.reasoning_content_length, 11);
}

TEST(OpenAiLlmClientTest, CanDisableReasoningContentFallback) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx(
        R"({"choices":[{"message":{"content":"","reasoning_content":"scratch output"},"finish_reason":"length"}],
            "model":"deepseek-v4-flash","usage":{"prompt_tokens":11,"completion_tokens":7}})");
    LlmCallConfig call;
    call.allow_reasoning_content_fallback = false;

    auto resp = cf.client->Chat("q", call);

    ASSERT_TRUE(resp.ok());
    EXPECT_EQ(resp.content, "");
    EXPECT_EQ(resp.content_source, "message.content");
    EXPECT_EQ(resp.finish_reason, "length");
    EXPECT_EQ(resp.model, "deepseek-v4-flash");
    EXPECT_EQ(resp.content_length, 0);
    EXPECT_EQ(resp.reasoning_content_length, 14);
}

TEST(OpenAiLlmClientTest, PrefersMessageContentOverReasoningContent) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx(
        R"({"choices":[{"message":{"content":"final","reasoning_content":"scratch"},"finish_reason":"stop"}]})");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    ASSERT_TRUE(resp.ok());
    EXPECT_EQ(resp.content, "final");
    EXPECT_EQ(resp.content_source, "message.content");
    EXPECT_EQ(resp.content_length, 5);
    EXPECT_EQ(resp.reasoning_content_length, 7);
}

TEST(OpenAiLlmClientTest, NetworkFailureIsUnavailableTransportToken) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::NetworkFail();
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(resp.status.code(), StatusCode::kUnavailable);
    EXPECT_EQ(resp.status.message().rfind(llm_tokens::kTransport, 0), 0u);
}

TEST(OpenAiLlmClientTest, NetworkFailureIncludesTransportDetailWhenPresent) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::NetworkFail();
    cf.fake->canned.transport_error = "Could not establish connection";
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(resp.status.code(), StatusCode::kUnavailable);
    EXPECT_NE(resp.status.message().find("Could not establish connection"), std::string::npos);
}

TEST(OpenAiLlmClientTest, RetriesTransportFailureThenParsesSuccess) {
    LlmClientConfig cfg;
    cfg.endpoint = "https://api.example.com/v1";
    cfg.api_key = "sk-test";
    cfg.max_retries = 2;
    auto cf = MakeClient(cfg);
    int calls = 0;
    cf.fake->responder = [&calls](const HttpRequest&) {
        ++calls;
        if (calls == 1) {
            HttpResponse failed = FakeHttpTransport::NetworkFail();
            failed.transport_error = "SSL connection failed";
            return failed;
        }
        return FakeHttpTransport::Json2xx(
            R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}],
                "usage":{"prompt_tokens":1,"completion_tokens":1}})");
    };

    auto resp = cf.client->Chat("q", LlmCallConfig{});

    ASSERT_TRUE(resp.ok());
    EXPECT_EQ(resp.content, "ok");
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(cf.fake->requests.size(), 2u);
}

TEST(OpenAiLlmClientTest, Http5xxIsUnavailableHttpToken) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Http(503, "upstream down");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(resp.status.code(), StatusCode::kUnavailable);
    EXPECT_EQ(resp.status.message().rfind(llm_tokens::kHttp, 0), 0u);
    EXPECT_NE(resp.status.message().find("503"), std::string::npos);
    EXPECT_EQ(resp.status.message().find("upstream down"), std::string::npos);
    EXPECT_EQ(resp.status.message().find("body_prefix="), std::string::npos);
}

TEST(OpenAiLlmClientTest, Http4xxIsInternalHttpToken) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Http(400, "bad\nrequest\tbody");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(resp.status.code(), StatusCode::kInternal);
    EXPECT_EQ(resp.status.message().rfind(llm_tokens::kHttp, 0), 0u);
    EXPECT_NE(resp.status.message().find("http_status=400"), std::string::npos);
    EXPECT_EQ(resp.status.message().find("bad"), std::string::npos);
    EXPECT_EQ(resp.status.message().find("body_prefix="), std::string::npos);
    EXPECT_EQ(resp.status.message().find('\n'), std::string::npos);
}

TEST(OpenAiLlmClientTest, RateLimitSurfacesRetryAfter) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::RateLimited(42);
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(resp.status.code(), StatusCode::kUnavailable);
    EXPECT_EQ(resp.status.message().rfind(llm_tokens::kRateLimit, 0), 0u);
    // Retry-After header value is carried out on finish_reason for the enricher.
    EXPECT_EQ(resp.finish_reason, "42");
}

TEST(OpenAiLlmClientTest, MalformedSuccessBodyIsBadBody) {
    auto cf = MakeClient();
    cf.fake->canned = FakeHttpTransport::Json2xx("not json at all");
    auto resp = cf.client->Chat("q", LlmCallConfig{});
    EXPECT_FALSE(resp.ok());
    EXPECT_EQ(resp.status.code(), StatusCode::kInternal);
    EXPECT_EQ(resp.status.message().rfind(llm_tokens::kBadBody, 0), 0u);
}

TEST(OpenAiLlmClientTest, FallsBackToDefaultModelWhenCallModelEmpty) {
    LlmClientConfig cfg;
    cfg.default_model = "my-default";
    auto cf = MakeClient(cfg);
    cf.fake->canned = FakeHttpTransport::Json2xx(
        R"({"choices":[{"message":{"content":"x"}}]})");  // no usage / model echo
    auto resp = cf.client->Chat("q", LlmCallConfig{});  // call.model empty
    ASSERT_TRUE(resp.ok());
    auto body = nlohmann::json::parse(cf.fake->last_request.body);
    EXPECT_EQ(body["model"], "my-default");
    EXPECT_EQ(resp.model, "my-default");  // server didn't echo → request model
    EXPECT_EQ(resp.prompt_tokens, 0);     // usage absent → 0
}

// A downstream feature consuming through the ILlmClient interface with a mock —
// no live endpoint. (Mirrors LlmEnricher's seam.)
TEST(MockLlmClientTest, DownstreamConsumesInterface) {
    MockLlmClient mock;
    ChatCompletionResponse canned;
    canned.content = R"({"entities":["Cortrix"]})";
    canned.finish_reason = "stop";
    canned.completion_tokens = 7;
    EXPECT_CALL(mock, Chat(_, _)).WillOnce(Return(canned));

    ILlmClient* llm = &mock;
    auto resp = llm->Chat("extract entities", LlmCallConfig{});
    ASSERT_TRUE(resp.ok());
    EXPECT_EQ(resp.content, R"({"entities":["Cortrix"]})");
    EXPECT_EQ(resp.completion_tokens, 7);
}

// Note: the real-endpoint integration test (live OpenAI/vLLM + CX_LLM_API_KEY,
// HTTPS via CPPHTTPLIB_OPENSSL_SUPPORT) is D3.5 deferred wiring; it lives under
// tests/integration/ once the transport build flag is enabled.

}  // namespace
}  // namespace cortrix::llm
