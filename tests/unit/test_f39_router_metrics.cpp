#include <gtest/gtest.h>

#include <string>

#include "cortrix/query/query_router_metrics.h"

// F39 coverage: the `query_router` subsystem metrics (§10) — 4 metrics
// (router_total counter{decision} / classifier_latency histogram /
// fallback_ratio gauge / compute_saved_seconds counter{path}), the enum labels,
// and the OpenMetrics renderer. Cardinality: NO ns_id label (D1 V3 decision 10 /
// OBS_SPEC §3.2).
namespace cortrix::query {
namespace {

using Decision = QueryRouterMetrics::Decision;
using SavedPath = QueryRouterMetrics::SavedPath;

class F39RouterMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { QueryRouterMetrics::Instance().ResetForTest(); }
    void TearDown() override { QueryRouterMetrics::Instance().ResetForTest(); }
};

TEST_F(F39RouterMetricsTest, DecisionToString) {
    EXPECT_STREQ(ToString(Decision::kSimple), "simple");
    EXPECT_STREQ(ToString(Decision::kComplex), "complex");
    EXPECT_STREQ(ToString(Decision::kChat), "chat");
    EXPECT_STREQ(ToString(Decision::kFallback), "fallback");
}

TEST_F(F39RouterMetricsTest, SavedPathToString) {
    EXPECT_STREQ(ToString(SavedPath::kSimple), "simple");
    EXPECT_STREQ(ToString(SavedPath::kChat), "chat");
}

TEST_F(F39RouterMetricsTest, DecisionCounterPerLabel) {
    auto& m = QueryRouterMetrics::Instance();
    m.RecordDecision(Decision::kSimple);
    m.RecordDecision(Decision::kSimple);
    m.RecordDecision(Decision::kComplex);
    m.RecordDecision(Decision::kChat);
    m.RecordDecision(Decision::kFallback);
    EXPECT_EQ(m.DecisionCount(Decision::kSimple), 2u);
    EXPECT_EQ(m.DecisionCount(Decision::kComplex), 1u);
    EXPECT_EQ(m.DecisionCount(Decision::kChat), 1u);
    EXPECT_EQ(m.DecisionCount(Decision::kFallback), 1u);
}

TEST_F(F39RouterMetricsTest, ClassifierLatencyHistogram) {
    auto& m = QueryRouterMetrics::Instance();
    m.ObserveClassifierLatency(0.0003);  // 0.3ms — rule path
    m.ObserveClassifierLatency(0.18);    // 180ms — LLM path
    m.ObserveClassifierLatency(0.7);     // 700ms
    EXPECT_EQ(m.ClassifierLatencyCount(), 3u);
    EXPECT_NEAR(m.ClassifierLatencySum(), 0.8803, 1e-3);
}

TEST_F(F39RouterMetricsTest, NegativeLatencyClampedToZero) {
    auto& m = QueryRouterMetrics::Instance();
    m.ObserveClassifierLatency(-1.0);
    EXPECT_EQ(m.ClassifierLatencyCount(), 1u);
    EXPECT_NEAR(m.ClassifierLatencySum(), 0.0, 1e-6);
}

TEST_F(F39RouterMetricsTest, FallbackRatioGaugeLastWriteWins) {
    auto& m = QueryRouterMetrics::Instance();
    m.SetFallbackRatio(0.012);
    m.SetFallbackRatio(0.048);  // overwrites
    EXPECT_NEAR(m.FallbackRatio(), 0.048, 1e-6);
}

TEST_F(F39RouterMetricsTest, ComputeSavedAccumulatesPerPath) {
    auto& m = QueryRouterMetrics::Instance();
    m.AddComputeSaved(SavedPath::kSimple, 0.5);
    m.AddComputeSaved(SavedPath::kSimple, 0.25);
    m.AddComputeSaved(SavedPath::kChat, 0.7);
    EXPECT_NEAR(m.ComputeSavedSeconds(SavedPath::kSimple), 0.75, 1e-3);
    EXPECT_NEAR(m.ComputeSavedSeconds(SavedPath::kChat), 0.7, 1e-3);
}

TEST_F(F39RouterMetricsTest, ComputeSavedNegativeClampedToZero) {
    auto& m = QueryRouterMetrics::Instance();
    m.AddComputeSaved(SavedPath::kSimple, -1.0);
    EXPECT_NEAR(m.ComputeSavedSeconds(SavedPath::kSimple), 0.0, 1e-6);
}

TEST_F(F39RouterMetricsTest, RenderOpenMetricsContainsAll4Metrics) {
    auto& m = QueryRouterMetrics::Instance();
    m.RecordDecision(Decision::kSimple);
    m.RecordDecision(Decision::kComplex);
    m.ObserveClassifierLatency(0.0004);
    m.SetFallbackRatio(0.03);
    m.AddComputeSaved(SavedPath::kSimple, 0.4);

    const std::string out = m.RenderOpenMetrics();

    // All 4 metric families present with their TYPE lines.
    EXPECT_NE(out.find("# TYPE cortrix_query_router_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_query_router_classifier_latency_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_query_router_fallback_ratio gauge"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_query_router_compute_saved_seconds counter"),
              std::string::npos);

    // decision labels rendered.
    EXPECT_NE(out.find("cortrix_query_router_total{decision=\"simple\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_query_router_total{decision=\"complex\"} 1"),
              std::string::npos);
    // histogram +Inf bucket + count.
    EXPECT_NE(out.find("cortrix_query_router_classifier_latency_seconds_bucket{le=\"+Inf\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_query_router_classifier_latency_seconds_count 1"),
              std::string::npos);
    // path label rendered.
    EXPECT_NE(out.find("cortrix_query_router_compute_saved_seconds{path=\"simple\"}"),
              std::string::npos);
}

// Cardinality guard (OBS_SPEC §3.2 / D1 V3 decision 10): the only labels ever
// emitted are the low-cardinality `decision` and `path` enums — never ns_id /
// tenant_id / user_id.
TEST_F(F39RouterMetricsTest, RenderHasNoHighCardinalityLabels) {
    auto& m = QueryRouterMetrics::Instance();
    m.RecordDecision(Decision::kChat);
    m.AddComputeSaved(SavedPath::kChat, 1.0);
    const std::string out = m.RenderOpenMetrics();
    EXPECT_EQ(out.find("ns_id"), std::string::npos);
    EXPECT_EQ(out.find("tenant_id"), std::string::npos);
    EXPECT_EQ(out.find("user_id"), std::string::npos);
}

TEST_F(F39RouterMetricsTest, ResetForTestClearsEverything) {
    auto& m = QueryRouterMetrics::Instance();
    m.RecordDecision(Decision::kSimple);
    m.ObserveClassifierLatency(0.01);
    m.SetFallbackRatio(0.5);
    m.AddComputeSaved(SavedPath::kChat, 2.0);
    m.ResetForTest();
    EXPECT_EQ(m.DecisionCount(Decision::kSimple), 0u);
    EXPECT_EQ(m.ClassifierLatencyCount(), 0u);
    EXPECT_NEAR(m.FallbackRatio(), 0.0, 1e-6);
    EXPECT_NEAR(m.ComputeSavedSeconds(SavedPath::kChat), 0.0, 1e-6);
}

}  // namespace
}  // namespace cortrix::query
