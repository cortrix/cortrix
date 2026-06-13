// S1.4 — CoreML EP auto-detection + coreml_fallback_total metric + active_ep
// gauge + CPU fallback. The live EP-selection branch only runs when a real ONNX
// session is created (needs a model file, absent offline → exercised at D3.5 /
// the W1 GB benchmark). The standalone-testable core is the metrics recorder +
// the config plumbing, covered here.
#include <gtest/gtest.h>

#include <string>

#include "cortrix/reranker/onnx_reranker.h"
#include "cortrix/reranker/reranker_metrics.h"

namespace cortrix::reranker {
namespace {

using Reason = RerankerMetrics::CoremlFallbackReason;

TEST(RerankerMetricsTest, CoremlFallbackCountsPerReason) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    EXPECT_EQ(m.CoremlFallbackCount(Reason::kEpInitFailed), 0u);

    m.RecordCoremlFallback(Reason::kEpInitFailed);
    m.RecordCoremlFallback(Reason::kEpInitFailed);
    m.RecordCoremlFallback(Reason::kUnsupportedPlatform);

    EXPECT_EQ(m.CoremlFallbackCount(Reason::kEpInitFailed), 2u);
    EXPECT_EQ(m.CoremlFallbackCount(Reason::kUnsupportedPlatform), 1u);
}

TEST(RerankerMetricsTest, ActiveEpGaugeRoundTrips) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    EXPECT_EQ(m.ActiveEp(), "cpu");  // default
    m.SetActiveEp("coreml");
    EXPECT_EQ(m.ActiveEp(), "coreml");
    m.SetActiveEp("cpu");
    EXPECT_EQ(m.ActiveEp(), "cpu");
}

TEST(RerankerMetricsTest, OpenMetricsRenderContainsCoremlAndEpLines) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    m.RecordCoremlFallback(Reason::kEpInitFailed);
    m.SetActiveEp("cpu");
    std::string text = m.RenderOpenMetrics();
    EXPECT_NE(text.find("cortrix_reranker_coreml_fallback_total{reason=\"ep_init_failed\"} 1"),
              std::string::npos);
    EXPECT_NE(text.find("cortrix_reranker_active_ep{ep=\"cpu\"} 1"), std::string::npos);
    EXPECT_NE(text.find("# TYPE cortrix_reranker_coreml_fallback_total counter"),
              std::string::npos);
}

TEST(RerankerMetricsTest, ReasonLabelStrings) {
    EXPECT_STREQ(ToString(Reason::kEpInitFailed), "ep_init_failed");
    EXPECT_STREQ(ToString(Reason::kUnsupportedPlatform), "unsupported_platform");
}

// --- D35-MET-04: queue_depth_current gauge + score_duration_seconds histogram ---

TEST(RerankerMetricsTest, QueueDepthGaugeLastWriteWins) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    EXPECT_EQ(m.QueueDepth(), 0);
    m.SetQueueDepth(7);
    EXPECT_EQ(m.QueueDepth(), 7);
    m.SetQueueDepth(3);  // gauge → last write wins
    EXPECT_EQ(m.QueueDepth(), 3);
    std::string text = m.RenderOpenMetrics();
    EXPECT_NE(text.find("# TYPE cortrix_reranker_queue_depth_current gauge"), std::string::npos);
    EXPECT_NE(text.find("cortrix_reranker_queue_depth_current 3"), std::string::npos);
}

TEST(RerankerMetricsTest, ScoreDurationHistogramBuckets) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    m.ObserveScoreDuration(0.04);   // <= 0.05
    m.ObserveScoreDuration(0.24);   // <= 0.5 (30-candidate batch ~240ms)
    m.ObserveScoreDuration(3.0);    // > 2.0, <= 5.0
    EXPECT_EQ(m.ScoreDurationCount(), 3u);
    EXPECT_NEAR(m.ScoreDurationSum(), 3.28, 1e-9);
    std::string text = m.RenderOpenMetrics();
    EXPECT_NE(text.find("cortrix_reranker_score_duration_seconds_bucket{le=\"0.05\"} 1"),
              std::string::npos);
    // cumulative at le="0.5": 0.04 + 0.24 = 2.
    EXPECT_NE(text.find("cortrix_reranker_score_duration_seconds_bucket{le=\"0.5\"} 2"),
              std::string::npos);
    EXPECT_NE(text.find("cortrix_reranker_score_duration_seconds_bucket{le=\"+Inf\"} 3"),
              std::string::npos);
    EXPECT_NE(text.find("cortrix_reranker_score_duration_seconds_count 3"), std::string::npos);
}

// --- config plumbing: use_coreml defaults to Auto + active_ep reflects it ---

TEST(RerankerCoremlTest, UseCoremlDefaultsToAuto) {
    RerankerConfig c;
    EXPECT_EQ(c.use_coreml, RerankerConfig::UseCoreML::kAuto);
}

TEST(RerankerCoremlTest, StubModeReportsCpuEpAndSetsGauge) {
    RerankerMetrics::Instance().ResetForTest();
    RerankerConfig c;  // empty model → stub mode, CPU
    OnnxReranker r(c, nullptr);
    ASSERT_TRUE(r.Init().ok());
    EXPECT_STREQ(r.active_ep(), "cpu");
    EXPECT_FALSE(r.is_coreml_active());
    EXPECT_EQ(RerankerMetrics::Instance().ActiveEp(), "cpu");
}

// The real CoreML auto/fail-fast/fallback branch needs a loaded ONNX model.
TEST(RerankerCoremlTest, LiveEpSelectionRequiresRealModel_SkipOffline) {
    GTEST_SKIP() << "CoreML EP auto-detect / fallback runs only with a real "
                    "bge-reranker-v2-m3 model (absent offline). Exercised at D3.5 "
                    "/ W1 Google Benchmark with the model fixture.";
}

}  // namespace
}  // namespace cortrix::reranker
