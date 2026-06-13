#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "cortrix/config/config.h"
#include "cortrix/onnx/onnx_cli.h"
#include "cortrix/onnx/onnx_error.h"
#include "cortrix/onnx/onnx_metrics.h"
#include "cortrix/onnx/runtime.h"
#include "cortrix/onnx/startup_validator.h"

namespace cortrix::onnx {
namespace {

class OnnxCliTest : public ::testing::Test {
 protected:
    void SetUp() override { OnnxMetrics::Instance().ResetForTest(); }
    void TearDown() override { OnnxMetrics::Instance().ResetForTest(); }
};

// TC-11: dry-run PASS → exit 0, stdout all-OK, stderr empty.
TEST_F(OnnxCliTest, RenderPassReport) {
    StartupValidator::ValidationReport report;
    report.status = Status::Ok();
    report.runtime_version = "1.17.1";
    report.runtime_major = 1;
    report.supported_opset_range = {7, 19};
    report.models.push_back({"/models/bge-m3/model.onnx", 17, true});

    std::ostringstream out, err;
    int code = RenderCheckOnnxReport(report, out, err);

    EXPECT_EQ(code, 0);
    EXPECT_NE(out.str().find("[OK] ONNX Runtime: 1.17.1"), std::string::npos);
    EXPECT_NE(out.str().find("opset 17 in [7, 19]"), std::string::npos);
    EXPECT_NE(out.str().find("All checks passed"), std::string::npos);
    EXPECT_TRUE(err.str().empty());
    // Outcome metric recorded as ok.
    EXPECT_EQ(OnnxMetrics::Instance().StartupValidationCount(
                  OnnxMetrics::StartupResult::kOk), 1u);
}

// A skipped (missing) model renders [SKIP] rather than [OK]/[FAIL].
TEST_F(OnnxCliTest, RenderPassReportWithSkippedModel) {
    StartupValidator::ValidationReport report;
    report.status = Status::Ok();
    report.runtime_version = "1.17.1";
    report.runtime_major = 1;
    report.supported_opset_range = {7, 19};
    report.models.push_back({"/models/absent.onnx", 0, false});

    std::ostringstream out, err;
    EXPECT_EQ(RenderCheckOnnxReport(report, out, err), 0);
    EXPECT_NE(out.str().find("[SKIP] Model /models/absent.onnx"), std::string::npos);
}

// TC-12: dry-run FAIL (opset incompatible) → exit 1, stderr has code +
// structured fields, stdout empty.
TEST_F(OnnxCliTest, RenderOpsetFailReport) {
    StartupValidator::ValidationReport report;
    report.runtime_version = "1.17.1";
    report.runtime_major = 1;
    report.supported_opset_range = {7, 19};
    report.models.push_back({"/models/bge-reranker-v2-m3.onnx", 20, true});
    report.status = OnnxStatus(OnnxErrorCode::kOpsetIncompatible,
                               "model opset 20 outside range");

    std::ostringstream out, err;
    int code = RenderCheckOnnxReport(report, out, err);

    EXPECT_EQ(code, 1);
    EXPECT_TRUE(out.str().empty());
    EXPECT_NE(err.str().find("[FAIL] CX_ERR_ONNX_OPSET_INCOMPATIBLE"), std::string::npos);
    EXPECT_NE(err.str().find("model_path: /models/bge-reranker-v2-m3.onnx"), std::string::npos);
    EXPECT_NE(err.str().find("model_opset: 20"), std::string::npos);
    EXPECT_NE(err.str().find("supported_opset_range: [7, 19]"), std::string::npos);
    EXPECT_NE(err.str().find("action: downgrade_model_or_upgrade_runtime"), std::string::npos);
    EXPECT_EQ(OnnxMetrics::Instance().StartupValidationCount(
                  OnnxMetrics::StartupResult::kOpsetIncompatible), 1u);
}

// FAIL (version mismatch) → stderr has the version-mismatch code + action.
TEST_F(OnnxCliTest, RenderVersionMismatchFailReport) {
    StartupValidator::ValidationReport report;
    report.runtime_version = "2.0.1";
    report.runtime_major = 2;
    report.supported_opset_range = {0, 0};
    report.status = OnnxStatus(OnnxErrorCode::kRuntimeVersionMismatch,
                               "loaded major 2 != compiled 1");

    std::ostringstream out, err;
    int code = RenderCheckOnnxReport(report, out, err);

    EXPECT_EQ(code, 1);
    EXPECT_NE(err.str().find("[FAIL] CX_ERR_ONNXRT_VERSION_MISMATCH"), std::string::npos);
    EXPECT_NE(err.str().find("current_version: 2.0.1"), std::string::npos);
    EXPECT_NE(err.str().find("action: replace_so_or_rebuild"), std::string::npos);
    EXPECT_EQ(OnnxMetrics::Instance().StartupValidationCount(
                  OnnxMetrics::StartupResult::kVersionMismatch), 1u);
}

// RunCheckOnnxCli end-to-end on a stub-only config (no model_path) → PASS, and
// sets the runtime version Info gauge when a runtime is linked.
TEST_F(OnnxCliTest, RunCheckOnnxCliStubConfigPasses) {
    CortrixConfig config;  // empty embedding.model_path → nothing registered
    std::ostringstream out, err;
    int code = RunCheckOnnxCli(config, out, err);
    EXPECT_EQ(code, 0);
    EXPECT_NE(out.str().find("All checks passed"), std::string::npos);
    if (Runtime::IsRuntimeLinked()) {
        // Info gauge populated from the loaded runtime version.
        std::string metrics = OnnxMetrics::Instance().RenderOpenMetrics();
        EXPECT_NE(metrics.find("cortrix_onnx_runtime_version_info{major=\"1\""),
                  std::string::npos);
    }
}

}  // namespace
}  // namespace cortrix::onnx
