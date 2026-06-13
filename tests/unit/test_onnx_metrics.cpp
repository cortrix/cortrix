#include <gtest/gtest.h>

#include <string>

#include "cortrix/onnx/onnx_metrics.h"

namespace cortrix::onnx {
namespace {

using SR = OnnxMetrics::StartupResult;

// The 4 metrics live in a process-wide singleton; reset before each test so
// counts are deterministic regardless of order / other suites.
class OnnxMetricsTest : public ::testing::Test {
 protected:
    void SetUp() override { OnnxMetrics::Instance().ResetForTest(); }
    void TearDown() override { OnnxMetrics::Instance().ResetForTest(); }
    OnnxMetrics& m() { return OnnxMetrics::Instance(); }
};

// ============================================================
// Label string mapping
// ============================================================

TEST_F(OnnxMetricsTest, StartupResultLabels) {
    EXPECT_STREQ(ToString(SR::kOk), "ok");
    EXPECT_STREQ(ToString(SR::kVersionMismatch), "version_mismatch");
    EXPECT_STREQ(ToString(SR::kOpsetIncompatible), "opset_incompatible");
    EXPECT_STREQ(ToString(SR::kInferenceCheckFailed), "inference_check_failed");
}

// ============================================================
// cortrix_onnx_startup_validation_total (TC-15)
// ============================================================

TEST_F(OnnxMetricsTest, StartupValidationCounterPerLabel) {
    m().RecordStartupValidation(SR::kOpsetIncompatible);
    m().RecordStartupValidation(SR::kOpsetIncompatible);
    m().RecordStartupValidation(SR::kOk);

    EXPECT_EQ(m().StartupValidationCount(SR::kOpsetIncompatible), 2u);
    EXPECT_EQ(m().StartupValidationCount(SR::kOk), 1u);
    EXPECT_EQ(m().StartupValidationCount(SR::kVersionMismatch), 0u);
}

// ============================================================
// cortrix_onnx_inference_failed_total
// ============================================================

TEST_F(OnnxMetricsTest, InferenceFailedCounter) {
    EXPECT_EQ(m().InferenceFailedCount(), 0u);
    m().RecordInferenceFailed();
    m().RecordInferenceFailed();
    EXPECT_EQ(m().InferenceFailedCount(), 2u);
}

// ============================================================
// cortrix_onnx_inference_duration_seconds (sum + count)
// ============================================================

TEST_F(OnnxMetricsTest, InferenceDurationSummary) {
    m().ObserveInferenceDuration(0.010);
    m().ObserveInferenceDuration(0.020);
    m().ObserveInferenceDuration(0.030);
    EXPECT_EQ(m().InferenceDurationCount(), 3u);
    EXPECT_NEAR(m().InferenceDurationSum(), 0.060, 1e-9);
}

// D35-MET-03: inference_duration_seconds now renders cumulative le buckets +Inf.
TEST_F(OnnxMetricsTest, InferenceDurationHistogramBuckets) {
    m().ObserveInferenceDuration(0.004);   // <= 0.005
    m().ObserveInferenceDuration(0.030);   // <= 0.05  (not <= 0.025)
    m().ObserveInferenceDuration(2.0);     // > 1.0 → +Inf only
    std::string t = m().RenderOpenMetrics();
    // le="0.005" sees only the 0.004 sample.
    EXPECT_NE(t.find("cortrix_onnx_inference_duration_seconds_bucket{le=\"0.005\"} 1"),
              std::string::npos);
    // le="0.05" is cumulative: 0.004 + 0.030 = 2.
    EXPECT_NE(t.find("cortrix_onnx_inference_duration_seconds_bucket{le=\"0.05\"} 2"),
              std::string::npos);
    // +Inf sees all three.
    EXPECT_NE(t.find("cortrix_onnx_inference_duration_seconds_bucket{le=\"+Inf\"} 3"),
              std::string::npos);
    EXPECT_NE(t.find("cortrix_onnx_inference_duration_seconds_count 3"), std::string::npos);
}

// ============================================================
// OpenMetrics text exposition (what F24 will serve at D3.5)
// ============================================================

TEST_F(OnnxMetricsTest, RenderContainsAllFourMetrics) {
    m().RecordStartupValidation(SR::kOk);
    m().RecordInferenceFailed();
    m().ObserveInferenceDuration(0.005);
    m().SetRuntimeVersionInfo(1, 17, 1);

    std::string text = m().RenderOpenMetrics();
    EXPECT_NE(text.find("cortrix_onnx_startup_validation_total"), std::string::npos);
    EXPECT_NE(text.find("cortrix_onnx_inference_failed_total"), std::string::npos);
    EXPECT_NE(text.find("cortrix_onnx_inference_duration_seconds"), std::string::npos);
    EXPECT_NE(text.find("cortrix_onnx_runtime_version_info"), std::string::npos);
}

TEST_F(OnnxMetricsTest, RenderStartupValidationHasResultLabel) {
    m().RecordStartupValidation(SR::kVersionMismatch);
    std::string text = m().RenderOpenMetrics();
    EXPECT_NE(text.find("cortrix_onnx_startup_validation_total{result=\"version_mismatch\"} 1"),
              std::string::npos);
    // The ok line is always present (0 when unrecorded) — Prometheus convention.
    EXPECT_NE(text.find("cortrix_onnx_startup_validation_total{result=\"ok\"} 0"),
              std::string::npos);
}

TEST_F(OnnxMetricsTest, RenderInferenceFailedHasRetryAttemptLabel) {
    m().RecordInferenceFailed();
    std::string text = m().RenderOpenMetrics();
    EXPECT_NE(text.find("cortrix_onnx_inference_failed_total{retry_attempt=\"1\"} 1"),
              std::string::npos);
}

// Info pattern: value always 1, version carried in labels (Agent reads version
// straight off the metric).
TEST_F(OnnxMetricsTest, RuntimeVersionInfoGauge) {
    m().SetRuntimeVersionInfo(1, 19, 0);
    std::string text = m().RenderOpenMetrics();
    EXPECT_NE(text.find("cortrix_onnx_runtime_version_info{major=\"1\",minor=\"19\",patch=\"0\"} 1"),
              std::string::npos);
}

// Until the version is set (pre-startup), the Info gauge has no sample line.
TEST_F(OnnxMetricsTest, RuntimeVersionInfoAbsentBeforeSet) {
    std::string text = m().RenderOpenMetrics();
    // The HELP/TYPE header is present, but no sample value line.
    EXPECT_EQ(text.find("cortrix_onnx_runtime_version_info{"), std::string::npos);
}

TEST_F(OnnxMetricsTest, TypeAndHelpLinesPresent) {
    std::string text = m().RenderOpenMetrics();
    EXPECT_NE(text.find("# TYPE cortrix_onnx_startup_validation_total counter"),
              std::string::npos);
    EXPECT_NE(text.find("# TYPE cortrix_onnx_runtime_version_info gauge"),
              std::string::npos);
}

}  // namespace
}  // namespace cortrix::onnx
