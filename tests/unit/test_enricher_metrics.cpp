#include "cortrix/spc_enricher/enricher_metrics.h"

#include <gtest/gtest.h>

#include <string>

#include "cortrix/spc_enricher/enricher_error.h"

namespace cortrix::spc {
namespace {

class EnricherMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { EnricherMetrics::Instance().ResetForTest(); }
    void TearDown() override { EnricherMetrics::Instance().ResetForTest(); }
    EnricherMetrics& m = EnricherMetrics::Instance();
};

TEST_F(EnricherMetricsTest, CircuitBreakerStateAndTrips) {
    EXPECT_EQ(m.CircuitBreakerState(), 0);
    m.SetCircuitBreakerState(1);
    EXPECT_EQ(m.CircuitBreakerState(), 1);
    m.RecordCircuitBreakerTrip();
    m.RecordCircuitBreakerTrip();
    EXPECT_EQ(m.CircuitBreakerTripsCount(), 2u);
}

TEST_F(EnricherMetricsTest, FailedTasksLabeledByErrorCode) {
    m.RecordFailedTask(EnricherErrorCode::kLlmTimeout);
    m.RecordFailedTask(EnricherErrorCode::kLlmTimeout);
    m.RecordFailedTask(EnricherErrorCode::kParse);
    EXPECT_EQ(m.FailedTasksCount(EnricherErrorCode::kLlmTimeout), 2u);
    EXPECT_EQ(m.FailedTasksCount(EnricherErrorCode::kParse), 1u);
    EXPECT_EQ(m.FailedTasksCount(EnricherErrorCode::kBudget), 0u);
}

TEST_F(EnricherMetricsTest, FallbackToNullByReason) {
    m.RecordFallbackToNull(EnricherMetrics::FallbackReason::kApiKeyMissing);
    m.RecordFallbackToNull(EnricherMetrics::FallbackReason::kCircuitOpen);
    EXPECT_EQ(m.FallbackToNullCount(EnricherMetrics::FallbackReason::kApiKeyMissing), 1u);
    EXPECT_EQ(m.FallbackToNullCount(EnricherMetrics::FallbackReason::kCircuitOpen), 1u);
    EXPECT_EQ(m.FallbackToNullCount(EnricherMetrics::FallbackReason::kBudgetExceeded), 0u);
}

TEST_F(EnricherMetricsTest, PerModelTokensAndCost) {
    m.RecordTokens("gpt-4o-mini", 100);
    m.RecordTokens("gpt-4o-mini", 50);
    m.RecordTokens("other", 7);
    EXPECT_EQ(m.TokensCount("gpt-4o-mini"), 150);
    EXPECT_EQ(m.TokensCount("other"), 7);
    EXPECT_EQ(m.TokensCount("missing"), 0);

    m.RecordCostMicroUsd("gpt-4o-mini", 1500);  // 0.0015 USD
    EXPECT_EQ(m.CostMicroUsd("gpt-4o-mini"), 1500);
}

TEST_F(EnricherMetricsTest, QueueDepthAndTruncated) {
    m.SetQueueDepth(7);
    EXPECT_EQ(m.QueueDepth(), 7);
    m.RecordTruncated();
    EXPECT_EQ(m.TruncatedCount(), 1u);
    m.RecordEndpointProbeFailed();
    EXPECT_EQ(m.EndpointProbeFailedCount(), 1u);
}

TEST_F(EnricherMetricsTest, RenderUsesCortrixPrefixAndLabels) {
    m.RecordFailedTask(EnricherErrorCode::kRateLimit);
    m.RecordTokens("gpt-4o-mini", 42);
    m.SetCircuitBreakerState(2);
    std::string out = m.RenderOpenMetrics();

    // V2 decision #3: cortrix_ prefix (no plugin prefix).
    EXPECT_NE(out.find("cortrix_enricher_circuit_breaker_state 2"), std::string::npos);
    EXPECT_EQ(out.find("pgcortrix_"), std::string::npos);
    // failed_tasks labeled by the CX_ERR_ENRICHER_* code.
    EXPECT_NE(out.find("cortrix_enricher_failed_tasks_total{reason=\"CX_ERR_ENRICHER_RATE_LIMIT\"} 1"),
              std::string::npos);
    // per-model tokens.
    EXPECT_NE(out.find("cortrix_enricher_tokens_total{model=\"gpt-4o-mini\"} 42"),
              std::string::npos);
    // every metric carries a TYPE line.
    EXPECT_NE(out.find("# TYPE cortrix_enricher_cost_usd_total counter"), std::string::npos);
}

TEST_F(EnricherMetricsTest, ResetForTestClearsAll) {
    m.RecordFailedTask(EnricherErrorCode::kParse);
    m.RecordTokens("m", 5);
    m.SetCircuitBreakerState(1);
    m.ResetForTest();
    EXPECT_EQ(m.FailedTasksCount(EnricherErrorCode::kParse), 0u);
    EXPECT_EQ(m.TokensCount("m"), 0);
    EXPECT_EQ(m.CircuitBreakerState(), 0);
}

}  // namespace
}  // namespace cortrix::spc
