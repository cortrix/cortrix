// S1.4 — CoreML EP auto-detection + coreml_fallback_total metric + active_ep
// gauge + CPU fallback. The live EP-selection branch only runs when a real ONNX
// session is created (needs a model file, absent offline → exercised at integration /
// the W1 GB benchmark). The standalone-testable core is the metrics recorder +
// the config plumbing, covered here.
#include <gtest/gtest.h>

#include <string>

#include "cortrix/reranker/onnx_reranker.h"
#include "cortrix/reranker/reranker_metrics.h"

namespace cortrix::reranker {
namespace {

using Reason = RerankerMetrics::CoremlFallbackReason;
using ProviderReason = RerankerMetrics::ProviderFallbackReason;

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

// ----MET-04: queue_depth_current gauge + score_duration_seconds histogram ---

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

// --- config plumbing: execution_provider defaults to Auto + active_ep reflects it ---

TEST(RerankerCoremlTest, ExecutionProviderDefaultsToAuto) {
    RerankerConfig c;
    EXPECT_EQ(c.execution_provider, "auto");
}

TEST(RerankerCoremlTest, LegacyAggregateFieldOrderStillCompiles) {
    RerankerConfig c{"model.onnx", "tokenizer.json",
                     RerankerConfig::UseCoreML::kForceFalse};
    EXPECT_EQ(c.use_coreml, RerankerConfig::UseCoreML::kForceFalse);
    EXPECT_EQ(c.execution_provider, "auto");
}

TEST(RerankerCoremlTest, DeprecatedForceFalseMapsToCpu) {
    RerankerConfig c;
    c.use_coreml = RerankerConfig::UseCoreML::kForceFalse;
    OnnxReranker r(c, nullptr);
    ASSERT_TRUE(r.Init().ok());
    EXPECT_STREQ(r.configured_ep(), "cpu");
    EXPECT_STREQ(r.active_ep(), "stub");
}

TEST(RerankerCoremlTest, DeprecatedAliasConflictsWithCanonicalProvider) {
    RerankerConfig c;
    c.execution_provider = "cuda";
    c.use_coreml = RerankerConfig::UseCoreML::kForceFalse;
    OnnxReranker r(c, nullptr);
    Status status = r.Init();
    EXPECT_FALSE(status.ok());
    EXPECT_NE(status.message().find("conflicts with deprecated use_coreml"),
              std::string::npos);
}

TEST(RerankerCoremlTest, StubModeReportsStubEpAndSetsGauge) {
    RerankerMetrics::Instance().ResetForTest();
    RerankerConfig c;  // empty model -> explicit development stub mode
    OnnxReranker r(c, nullptr);
    ASSERT_TRUE(r.Init().ok());
    EXPECT_STREQ(r.active_ep(), "stub");
    EXPECT_FALSE(r.is_coreml_active());
    EXPECT_EQ(RerankerMetrics::Instance().ActiveEp(), "stub");
}

// The real CoreML auto/fail-fast/fallback branch needs a loaded ONNX model.
TEST(RerankerCoremlTest, LiveEpSelectionRequiresRealModel_SkipOffline) {
    GTEST_SKIP() << "CoreML EP auto-detect / fallback runs only with a real "
                    "bge-reranker-v2-m3 model (absent offline). Exercise this in "
                    "the platform capability smoke with the model fixture.";
}

TEST(RerankerCoremlTest, ConfiguredAndActiveEpRoundTripsForFallbackStates) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    m.SetConfiguredEp("cuda");
    m.SetActiveEp("cpu");
    EXPECT_EQ(m.ConfiguredEp(), "cuda");
    EXPECT_EQ(m.ActiveEp(), "cpu");
    m.SetConfiguredEp("auto");
    m.SetActiveEp("stub");
    EXPECT_EQ(m.ConfiguredEp(), "auto");
    EXPECT_EQ(m.ActiveEp(), "stub");
}

TEST(RerankerCoremlTest, ProviderFallbackMetricsDistinguishSourceAndReason) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    m.RecordProviderFallback("coreml",
                             ProviderReason::kSessionInitFailed);
    m.RecordProviderFallback("coreml",
                             ProviderReason::kSessionInitFailed);
    m.RecordProviderFallback("cuda",
                             ProviderReason::kEpInitFailed);
    EXPECT_EQ(m.ProviderFallbackCount("coreml", ProviderReason::kSessionInitFailed), 2u);
    EXPECT_EQ(m.ProviderFallbackCount("cuda", ProviderReason::kEpInitFailed), 1u);
}

TEST(RerankerCoremlTest, ProviderInitFailureMetricsCoverCpuCoremlAndCuda) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    m.RecordProviderInitFailure("cpu", ProviderReason::kSessionInitFailed);
    m.RecordProviderInitFailure("coreml", ProviderReason::kEpInitFailed);
    m.RecordProviderInitFailure("cuda", ProviderReason::kSessionInitFailed);
    EXPECT_EQ(m.ProviderInitFailureCount("cpu", ProviderReason::kSessionInitFailed), 1u);
    EXPECT_EQ(m.ProviderInitFailureCount("coreml", ProviderReason::kEpInitFailed), 1u);
    EXPECT_EQ(m.ProviderInitFailureCount("cuda", ProviderReason::kSessionInitFailed), 1u);
}

TEST(RerankerCoremlTest, OpenMetricsRenderIncludesConfiguredActiveAndFallback) {
    auto& m = RerankerMetrics::Instance();
    m.ResetForTest();
    m.RecordProviderFallback("coreml", ProviderReason::kEpInitFailed);
    m.RecordProviderInitFailure("cuda", ProviderReason::kSessionInitFailed);
    m.SetConfiguredEp("cuda");
    m.SetActiveEp("stub");
    std::string text = m.RenderOpenMetrics();
    EXPECT_NE(text.find("cortrix_reranker_configured_ep{ep=\"cuda\"} 1"), std::string::npos);
    EXPECT_NE(text.find("cortrix_reranker_active_ep{ep=\"stub\"} 1"), std::string::npos);
    EXPECT_NE(text.find("cortrix_reranker_ep_fallback_total{from=\"coreml\",to=\"cpu\","
                        "reason=\"ep_init_failed\"} 1"), std::string::npos);
    EXPECT_NE(text.find("cortrix_reranker_ep_init_failure_total{provider=\"cuda\","
                        "reason=\"session_init_failed\"} 1"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::reranker
