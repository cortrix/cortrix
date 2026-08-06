// S2.5 — circuit breaker metric (state gauge / trips counter / failed_tasks) +
// status JSONB + WARN/INFO log + alert rules.
#include <gtest/gtest.h>

#include <string>

#include "cortrix/reranker/circuit_breaker.h"
#include "cortrix/reranker/reranker_metrics.h"
#include "cortrix/reranker/reranker_status.h"

namespace cortrix::reranker {
namespace {

// --- status JSONB ---

TEST(RerankerStatusTest, ClosedStateJson) {
    auto j = RerankerStatusReporter::BuildStatusJson(
        CircuitBreakerState::kClosed, -1, "", "cpu");
    EXPECT_EQ(j["reranker_status"], "ok");
    EXPECT_TRUE(j["circuit_breaker_open_since"].is_null());
    EXPECT_TRUE(j["last_failure_reason"].is_null());
    EXPECT_EQ(j["active_ep"], "cpu");
}

TEST(RerankerStatusTest, OpenStateJsonHasTimestampAndReason) {
    // 1700000000000 ms = 2023-11-14T22:13:20Z.
    auto j = RerankerStatusReporter::BuildStatusJson(
        CircuitBreakerState::kOpen, 1700000000000LL, "ONNX OOM", "coreml");
    EXPECT_EQ(j["reranker_status"], "circuit_open");
    EXPECT_EQ(j["circuit_breaker_open_since"], "2023-11-14T22:13:20Z");
    EXPECT_EQ(j["last_failure_reason"], "ONNX OOM");
    EXPECT_EQ(j["active_ep"], "coreml");
}

TEST(RerankerStatusTest, HalfOpenStateJsonIsDegraded) {
    auto j = RerankerStatusReporter::BuildStatusJson(
        CircuitBreakerState::kHalfOpen, 1700000000000LL, "timeout", "cpu");
    EXPECT_EQ(j["reranker_status"], "degraded");
    EXPECT_FALSE(j["circuit_breaker_open_since"].is_null());
}

TEST(RerankerStatusTest, Iso8601FormatsAndHandlesNegative) {
    EXPECT_EQ(RerankerStatusReporter::ToIso8601Utc(1700000000000LL),
              "2023-11-14T22:13:20Z");
    EXPECT_EQ(RerankerStatusReporter::ToIso8601Utc(-1), "");
}

// --- metric wiring via OnStateChange ---

TEST(RerankerStatusTest, OnStateChangeUpdatesGaugeAndTripsCounter) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();

    RerankerStatusReporter::OnCircuitStateChange(CircuitBreakerState::kOpen, 30);
    EXPECT_EQ(m.CircuitBreakerState(), 1);
    EXPECT_EQ(m.CircuitBreakerTripsCount(), 1u);  // trip counted on open

    RerankerStatusReporter::OnCircuitStateChange(CircuitBreakerState::kHalfOpen, 30);
    EXPECT_EQ(m.CircuitBreakerState(), 2);
    EXPECT_EQ(m.CircuitBreakerTripsCount(), 1u);  // half-open is not a trip

    RerankerStatusReporter::OnCircuitStateChange(CircuitBreakerState::kClosed, 30);
    EXPECT_EQ(m.CircuitBreakerState(), 0);
    EXPECT_EQ(m.CircuitBreakerTripsCount(), 1u);
}

// --- end-to-end: breaker transitions drive the metric through the reporter ---

TEST(RerankerStatusTest, BreakerWiredToReporterDrivesMetricEndToEnd) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();

    CircuitBreaker cb(3, 30);
    cb.OnStateChange([](CircuitBreakerState s) {
        RerankerStatusReporter::OnCircuitStateChange(s, 30);
    });

    EXPECT_EQ(m.CircuitBreakerState(), 0);  // starts closed
    for (int i = 0; i < 3; ++i) cb.RecordFailure();  // trips open
    EXPECT_EQ(m.CircuitBreakerState(), 1);
    EXPECT_EQ(m.CircuitBreakerTripsCount(), 1u);
}

TEST(RerankerStatusTest, FailedTasksCounterPerReason) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    m.RecordFailedTask(RerankerMetrics::FailedTaskReason::kTimeout);
    m.RecordFailedTask(RerankerMetrics::FailedTaskReason::kOnnxException);
    m.RecordFailedTask(RerankerMetrics::FailedTaskReason::kTimeout);
    EXPECT_EQ(m.FailedTasksCount(RerankerMetrics::FailedTaskReason::kTimeout), 2u);
    EXPECT_EQ(m.FailedTasksCount(RerankerMetrics::FailedTaskReason::kOnnxException), 1u);
    EXPECT_EQ(m.FailedTasksCount(RerankerMetrics::FailedTaskReason::kOom), 0u);
}

// --- alert rules ---

TEST(RerankerStatusTest, AlertRulesMatchSpec) {
    EXPECT_NE(RerankerStatusReporter::AlertRuleCircuitOpen5m()
                  .find("cortrix_reranker_circuit_breaker_state == 1"),
              std::string::npos);
    EXPECT_NE(RerankerStatusReporter::AlertRuleCircuitOpen5m().find("5m"),
              std::string::npos);
    EXPECT_NE(RerankerStatusReporter::AlertRuleFrequentTrips1h()
                  .find("cortrix_reranker_circuit_breaker_trips_total"),
              std::string::npos);
    EXPECT_NE(RerankerStatusReporter::AlertRuleFrequentTrips1h().find(">= 3"),
              std::string::npos);
}

}  // namespace
}  // namespace cortrix::reranker
