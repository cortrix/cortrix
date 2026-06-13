#include <gtest/gtest.h>

#include <string>

#include "cortrix/retrieval/crag_metrics.h"

// F37 S6 coverage: the `crag` subsystem metrics (§10) — 4 metrics
// (evaluation_total counter / classifier_latency histogram / fallback_ratio gauge
// / incorrect_ratio gauge), the decision enum label, and the OpenMetrics renderer.
// Cardinality: NO ns_id label (D1 V3 decision 10 / OBS_SPEC §3.2).
namespace cortrix::retrieval {
namespace {

using Decision = CragMetrics::Decision;

class CragMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { CragMetrics::Instance().ResetForTest(); }
    void TearDown() override { CragMetrics::Instance().ResetForTest(); }
};

TEST_F(CragMetricsTest, DecisionToString) {
    EXPECT_STREQ(ToString(Decision::kCorrect), "correct");
    EXPECT_STREQ(ToString(Decision::kAmbiguous), "ambiguous");
    EXPECT_STREQ(ToString(Decision::kIncorrect), "incorrect");
    EXPECT_STREQ(ToString(Decision::kFallback), "fallback");
}

TEST_F(CragMetricsTest, EvaluationCounterPerDecision) {
    auto& m = CragMetrics::Instance();
    m.RecordEvaluation(Decision::kCorrect);
    m.RecordEvaluation(Decision::kCorrect);
    m.RecordEvaluation(Decision::kIncorrect);
    m.RecordEvaluation(Decision::kFallback);
    EXPECT_EQ(m.EvaluationCount(Decision::kCorrect), 2u);
    EXPECT_EQ(m.EvaluationCount(Decision::kAmbiguous), 0u);
    EXPECT_EQ(m.EvaluationCount(Decision::kIncorrect), 1u);
    EXPECT_EQ(m.EvaluationCount(Decision::kFallback), 1u);
}

TEST_F(CragMetricsTest, ClassifierLatencyHistogram) {
    auto& m = CragMetrics::Instance();
    m.ObserveClassifierLatency(0.003);  // 3ms
    m.ObserveClassifierLatency(0.015);  // 15ms
    m.ObserveClassifierLatency(0.5);    // 500ms
    EXPECT_EQ(m.ClassifierLatencyCount(), 3u);
    EXPECT_NEAR(m.ClassifierLatencySum(), 0.518, 1e-3);
}

TEST_F(CragMetricsTest, NegativeLatencyClampedToZero) {
    auto& m = CragMetrics::Instance();
    m.ObserveClassifierLatency(-1.0);
    EXPECT_EQ(m.ClassifierLatencyCount(), 1u);
    EXPECT_NEAR(m.ClassifierLatencySum(), 0.0, 1e-6);
}

TEST_F(CragMetricsTest, RatioGaugesLastWriteWins) {
    auto& m = CragMetrics::Instance();
    m.SetFallbackRatio(0.012);
    m.SetFallbackRatio(0.018);  // overwrites
    EXPECT_NEAR(m.FallbackRatio(), 0.018, 1e-5);
    m.SetIncorrectRatio(0.25);
    EXPECT_NEAR(m.IncorrectRatio(), 0.25, 1e-5);
}

TEST_F(CragMetricsTest, RenderOpenMetricsShapeAndNoHighCardinalityLabel) {
    auto& m = CragMetrics::Instance();
    m.RecordEvaluation(Decision::kCorrect);
    m.RecordEvaluation(Decision::kIncorrect);
    m.ObserveClassifierLatency(0.004);
    m.SetFallbackRatio(0.01);
    m.SetIncorrectRatio(0.2);

    std::string out = m.RenderOpenMetrics();

    // All 4 metric names present with correct TYPE.
    EXPECT_NE(out.find("# TYPE cortrix_crag_evaluation_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_crag_classifier_latency_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_crag_fallback_ratio gauge"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_crag_incorrect_ratio gauge"), std::string::npos);

    // decision enum labels rendered.
    EXPECT_NE(out.find("cortrix_crag_evaluation_total{decision=\"correct\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_crag_evaluation_total{decision=\"incorrect\"} 1"),
              std::string::npos);

    // Histogram emits _bucket / _sum / _count + a +Inf bucket.
    EXPECT_NE(out.find("cortrix_crag_classifier_latency_seconds_bucket{le=\"+Inf\"}"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_crag_classifier_latency_seconds_count 1"),
              std::string::npos);

    // 🚨 Cardinality: NO ns_id / tenant_id / user_id label anywhere (OBS_SPEC §3.2).
    EXPECT_EQ(out.find("ns_id"), std::string::npos);
    EXPECT_EQ(out.find("tenant_id"), std::string::npos);
    EXPECT_EQ(out.find("user_id"), std::string::npos);
}

TEST_F(CragMetricsTest, ResetForTestClearsAll) {
    auto& m = CragMetrics::Instance();
    m.RecordEvaluation(Decision::kCorrect);
    m.ObserveClassifierLatency(0.01);
    m.SetFallbackRatio(0.05);
    m.ResetForTest();
    EXPECT_EQ(m.EvaluationCount(Decision::kCorrect), 0u);
    EXPECT_EQ(m.ClassifierLatencyCount(), 0u);
    EXPECT_NEAR(m.FallbackRatio(), 0.0, 1e-6);
}

}  // namespace
}  // namespace cortrix::retrieval
