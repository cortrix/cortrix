#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cortrix/llm/i_llm_client.h"
#include "cortrix/llm/llm_error_tokens.h"
#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/enricher_error.h"
#include "cortrix/spc_enricher/enricher_metrics.h"
#include "cortrix/spc/parser.h"

namespace cortrix::spc {
namespace {

// Hand-written configurable ILlmClient for resilience tests (timeout / retry /
// breaker). gmock can't easily model "sleep then return", so this fake does.
class ConfigurableLlmClient : public llm::ILlmClient {
public:
    std::atomic<int> calls{0};
    int sleep_ms = 0;                 // per-call delay (for timeout tests)
    int fail_first_n = 0;             // first N calls fail (transient), then succeed
    bool always_fail = false;         // every call fails
    std::string fail_token = llm::llm_tokens::kHttp;  // which neutral token to fail with
    std::string ok_content = R"({"0":{"summary":"ok","score":0.5}})";

    llm::ChatCompletionResponse Chat(const std::string&,
                                     const llm::LlmCallConfig&) override {
        int call_no = ++calls;
        if (sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        llm::ChatCompletionResponse r;
        const bool fail = always_fail || call_no <= fail_first_n;
        if (fail) {
            r.status = Status::Unavailable(fail_token + ": synthetic failure");
            return r;
        }
        r.status = Status::Ok();
        r.content = ok_content;
        r.prompt_tokens = 10;
        r.completion_tokens = 5;
        return r;
    }
};

EnricherConfig FastCfg() {
    EnricherConfig cfg;
    cfg.type = EnricherType::kLlm;
    cfg.enabled = true;
    cfg.model = "gpt-4o-mini";
    cfg.batch_size = 8;
    cfg.workers = 2;
    cfg.queue_size = 16;
    cfg.task_timeout_ms = 200;          // small so timeout tests are fast
    cfg.http_retry_backoff_ms = 1;      // seconds-level default would stall tests
    cfg.circuit_breaker_enabled = true;
    cfg.circuit_breaker_threshold = 3;  // trip quickly for the test
    cfg.circuit_breaker_cooldown_sec = 60;
    return cfg;
}

std::vector<ChunkContext> OneChunk(const DocumentMetadata& meta) {
    ChunkContext c;
    c.chunk_text = "hello";
    c.doc_metadata = &meta;
    return {c};
}

class ResilienceTest : public ::testing::Test {
protected:
    void SetUp() override { EnricherMetrics::Instance().ResetForTest(); }
    void TearDown() override { EnricherMetrics::Instance().ResetForTest(); }
    DocumentMetadata meta;
};

TEST_F(ResilienceTest, Http5xxRetriedThreeTimesThenFails) {
    auto client = std::make_shared<ConfigurableLlmClient>();
    client->always_fail = true;
    client->fail_token = llm::llm_tokens::kHttp;  // 5xx-style → retryable

    auto cfg = FastCfg();
    cfg.circuit_breaker_enabled = false;  // isolate retry behavior
    LlmEnricher enr(cfg, client);
    auto results = enr.EnrichBatch(OneChunk(meta));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].status, static_cast<int>(EnricherErrorCode::kLlmApi));
    // LLM_API retry 3  times → exactly 3 Chat calls for the single batch.
    EXPECT_EQ(client->calls.load(), 3);
}

TEST_F(ResilienceTest, RateLimitNotBusyRetried) {
    auto client = std::make_shared<ConfigurableLlmClient>();
    client->always_fail = true;
    client->fail_token = llm::llm_tokens::kRateLimit;  // 429 → no busy retry

    auto cfg = FastCfg();
    cfg.circuit_breaker_enabled = false;
    LlmEnricher enr(cfg, client);
    auto results = enr.EnrichBatch(OneChunk(meta));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].status, static_cast<int>(EnricherErrorCode::kRateLimit));
    EXPECT_EQ(client->calls.load(), 1);  // 429 returned immediately, no retry loop
}

TEST_F(ResilienceTest, TransientThenSuccessRecovers) {
    auto client = std::make_shared<ConfigurableLlmClient>();
    client->fail_first_n = 2;  // calls 1,2 fail (5xx), call 3 succeeds
    client->fail_token = llm::llm_tokens::kHttp;

    auto cfg = FastCfg();
    cfg.circuit_breaker_enabled = false;
    LlmEnricher enr(cfg, client);
    auto results = enr.EnrichBatch(OneChunk(meta));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].ok());  // 3rd attempt within the same batch succeeded
    EXPECT_EQ(results[0].summary, "ok");
    EXPECT_EQ(client->calls.load(), 3);
}

TEST_F(ResilienceTest, TaskTimeoutProducesTimeoutErrorAndRetriesOnce) {
    auto client = std::make_shared<ConfigurableLlmClient>();
    client->sleep_ms = 600;  // > task_timeout_ms (200) → pool-level timeout

    auto cfg = FastCfg();
    cfg.circuit_breaker_enabled = false;
    LlmEnricher enr(cfg, client);
    auto results = enr.EnrichBatch(OneChunk(meta));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].status, static_cast<int>(EnricherErrorCode::kLlmTimeout));
    EXPECT_TRUE(results[0].error_meta.retryable);
    EXPECT_EQ(results[0].error_meta.category, agent_friendly::ErrorCategory::kTimeout);
    EXPECT_EQ(EnricherMetrics::Instance().FailedTasksCount(EnricherErrorCode::kLlmTimeout), 1u);
}

TEST_F(ResilienceTest, CircuitBreakerOpensAndDegradesWithoutCalling) {
    auto client = std::make_shared<ConfigurableLlmClient>();
    client->always_fail = true;
    client->fail_token = llm::llm_tokens::kHttp;

    auto cfg = FastCfg();  // breaker threshold = 3
    LlmEnricher enr(cfg, client);

    // Each EnrichBatch makes 3 Chat attempts (retries) but records ONE breaker
    // failure per batch. 3 failing batches → breaker trips to open.
    for (int i = 0; i < 3; ++i) {
        auto r = enr.EnrichBatch(OneChunk(meta));
        EXPECT_EQ(r[0].status, static_cast<int>(EnricherErrorCode::kLlmApi));
    }
    int calls_before = client->calls.load();
    EXPECT_GT(calls_before, 0);

    // Breaker now open → next batch degrades immediately, no further Chat calls.
    auto degraded = enr.EnrichBatch(OneChunk(meta));
    ASSERT_EQ(degraded.size(), 1u);
    EXPECT_FALSE(degraded[0].ok());
    EXPECT_EQ(client->calls.load(), calls_before);  // no new calls while open
    EXPECT_GE(EnricherMetrics::Instance().CircuitBreakerTripsCount(), 1u);
    EXPECT_GE(EnricherMetrics::Instance().FallbackToNullCount(
                  EnricherMetrics::FallbackReason::kCircuitOpen), 1u);
}

TEST_F(ResilienceTest, TransportBackoffEngagesBetweenRetries) {
    // Contract: the default base is seconds-level (addendum A-part).
    EXPECT_EQ(EnricherConfig{}.http_retry_backoff_ms, 5000);

    auto client = std::make_shared<ConfigurableLlmClient>();
    client->fail_first_n = 2;  // calls 1,2 fail → two backoff gaps → call 3 succeeds
    client->fail_token = llm::llm_tokens::kTransport;

    auto cfg = FastCfg();
    cfg.circuit_breaker_enabled = false;
    cfg.http_retry_backoff_ms = 40;  // schedule: 40ms + 80ms = 120ms of sleeps
    LlmEnricher enr(cfg, client);

    const auto t0 = std::chrono::steady_clock::now();
    auto results = enr.EnrichBatch(OneChunk(meta));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].ok());
    EXPECT_EQ(client->calls.load(), 3);
    // Sleeps guarantee a lower bound (no upper bound → not flaky).
    EXPECT_GE(elapsed_ms, 100);
}

TEST_F(ResilienceTest, SuccessRecordsTokensMetric) {
    auto client = std::make_shared<ConfigurableLlmClient>();  // succeeds, 15 tokens
    auto cfg = FastCfg();
    LlmEnricher enr(cfg, client);
    auto results = enr.EnrichBatch(OneChunk(meta));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].ok());
    EXPECT_EQ(results[0].token_count, 15);
    EXPECT_EQ(EnricherMetrics::Instance().TokensCount("gpt-4o-mini"), 15);
}

TEST_F(ResilienceTest, BudgetCapRefusesAfterSpend) {
    // Tiny $1 cap. The client reports a large prompt_tokens so one successful call
    // accrues > $1, tripping the budget gate for the next batch.
    auto client = std::make_shared<ConfigurableLlmClient>();
    client->ok_content = R"({"0":{"summary":"ok","score":0.5}})";
    // Override the canned token usage: 1M gpt-4o-mini input ≈ $0.15; 10M ≈ $1.5 > $1.
    // ConfigurableLlmClient returns fixed 10/5; instead drive cost via a custom
    // client below. Simpler: use a model with high price via config.model unknown.
    auto cfg = FastCfg();
    cfg.budget_cap_usd = 1;

    // Use a client returning large usage to exceed $1 in one call.
    struct BigUsageClient : public llm::ILlmClient {
        llm::ChatCompletionResponse Chat(const std::string&,
                                         const llm::LlmCallConfig&) override {
            llm::ChatCompletionResponse r;
            r.status = Status::Ok();
            r.content = R"({"0":{"summary":"ok","score":0.5}})";
            r.prompt_tokens = 20'000'000;  // gpt-4o-mini: 20M*150/1000 = 3,000,000 micro = $3
            r.completion_tokens = 0;
            return r;
        }
    };
    auto big = std::make_shared<BigUsageClient>();
    LlmEnricher enr(cfg, big);

    // First batch: allowed (budget starts at 0), succeeds, accrues ~$3.
    auto r1 = enr.EnrichBatch(OneChunk(meta));
    ASSERT_EQ(r1.size(), 1u);
    EXPECT_TRUE(r1[0].ok());

    // Second batch: budget now over cap → refused with CX_ERR_ENRICHER_BUDGET.
    auto r2 = enr.EnrichBatch(OneChunk(meta));
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_EQ(r2[0].status, static_cast<int>(EnricherErrorCode::kBudget));
    EXPECT_FALSE(r2[0].error_meta.retryable);  // quota = not retryable
    EXPECT_EQ(r2[0].error_meta.category, agent_friendly::ErrorCategory::kQuota);
    EXPECT_GE(EnricherMetrics::Instance().FallbackToNullCount(
                  EnricherMetrics::FallbackReason::kBudgetExceeded), 1u);
}

}  // namespace
}  // namespace cortrix::spc
