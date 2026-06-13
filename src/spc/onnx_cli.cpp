#include "cortrix/onnx/onnx_cli.h"

#include <cstdio>
#include <string>

#include "cortrix/config/config.h"
#include "cortrix/onnx/onnx_error.h"
#include "cortrix/onnx/onnx_metrics.h"
#include "cortrix/onnx/runtime.h"

namespace cortrix::onnx {

namespace {

// Map a failed ValidationReport to the startup-validation metric outcome label,
// using the CX_ERR_* token carried in the Status message (the report's Status
// is built by OnnxStatus(), which prefixes the code).
OnnxMetrics::StartupResult OutcomeFromStatus(const Status& status) {
    if (status.ok()) return OnnxMetrics::StartupResult::kOk;
    const std::string& m = status.message();
    if (m.find("CX_ERR_ONNXRT_VERSION_MISMATCH") != std::string::npos) {
        return OnnxMetrics::StartupResult::kVersionMismatch;
    }
    if (m.find("CX_ERR_ONNX_OPSET_INCOMPATIBLE") != std::string::npos) {
        return OnnxMetrics::StartupResult::kOpsetIncompatible;
    }
    return OnnxMetrics::StartupResult::kInferenceCheckFailed;
}

}  // namespace

int RenderCheckOnnxReport(const StartupValidator::ValidationReport& report,
                          std::ostream& out, std::ostream& err) {
    // Record the outcome (TC-15) — the dry-run is the same validation an Agent
    // would scrape after an upgrade.
    OnnxMetrics::Instance().RecordStartupValidation(OutcomeFromStatus(report.status));

    if (report.status.ok()) {
        out << "[OK] ONNX Runtime: " << report.runtime_version
            << " (major=" << report.runtime_major
            << ", matches CMake expectation)\n";
        for (const auto& m : report.models) {
            if (m.checked) {
                out << "[OK] Model " << m.model_path << ": opset " << m.model_opset
                    << " in [" << report.supported_opset_range.first << ", "
                    << report.supported_opset_range.second << "]\n";
            } else {
                out << "[SKIP] Model " << m.model_path << ": not present on disk\n";
            }
        }
        out << "[OK] All checks passed. Safe to restart cortrix-server.\n";
        return 0;
    }

    // FAIL: surface the CX_ERR_* code + the structured fields the report carries.
    const OnnxMetrics::StartupResult outcome = OutcomeFromStatus(report.status);
    if (outcome == OnnxMetrics::StartupResult::kVersionMismatch) {
        err << "[FAIL] CX_ERR_ONNXRT_VERSION_MISMATCH\n";
        err << "  current_version: " << report.runtime_version << "\n";
        err << "  current_major: " << report.runtime_major << "\n";
        err << "  expected_major_version: " << Runtime::GetCompiledMajorVersion() << "\n";
        err << "  action: replace_so_or_rebuild\n";
    } else if (outcome == OnnxMetrics::StartupResult::kOpsetIncompatible) {
        err << "[FAIL] CX_ERR_ONNX_OPSET_INCOMPATIBLE\n";
        // The offending model is the last one in the report (validation stops at
        // the first failure); surface its detail.
        if (!report.models.empty()) {
            const auto& bad = report.models.back();
            err << "  model_path: " << bad.model_path << "\n";
            if (bad.checked) err << "  model_opset: " << bad.model_opset << "\n";
        }
        err << "  supported_opset_range: [" << report.supported_opset_range.first
            << ", " << report.supported_opset_range.second << "]\n";
        err << "  action: downgrade_model_or_upgrade_runtime\n";
    } else {
        err << "[FAIL] " << report.status.message() << "\n";
    }
    return 1;
}

int RunCheckOnnxCli(const CortrixConfig& config, std::ostream& out, std::ostream& err) {
    // Reflect the loaded runtime version into the Info gauge (so a subsequent
    // scrape shows it even on the dry-run path).
    if (Runtime::IsRuntimeLinked()) {
        // Parse "major.minor.patch" loosely; missing components default to 0.
        const std::string v = Runtime::GetRuntimeVersion();
        int maj = 0, min = 0, pat = 0;
        std::sscanf(v.c_str(), "%d.%d.%d", &maj, &min, &pat);
        OnnxMetrics::Instance().SetRuntimeVersionInfo(maj, min, pat);
    }
    auto cfg = StartupValidator::CollectRegisteredOnnxModels(config);
    auto report = StartupValidator::ValidateVerbose(cfg);
    return RenderCheckOnnxReport(report, out, err);
}

}  // namespace cortrix::onnx
