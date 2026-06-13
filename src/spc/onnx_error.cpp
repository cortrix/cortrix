#include "cortrix/onnx/onnx_error.h"

#include <utility>

namespace cortrix::onnx {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (F22 §8.3). Defined as function-local statics so
// each returns a stable reference. The switches below are intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without
// a row" into a warning, which the project treats as a build failure — the
// registry can't silently drift from the enum.
//
// retry_after_ms: only the run-time INFERENCE_FAILED is retryable (transient,
// 200ms per §8.3 / §10 flow). The two startup errors are permanent — the user
// must swap the `.so` or rebuild, so retry is meaningless.
constexpr OnnxErrorInfo kRuntimeVersionMismatch{
    "CX_ERR_ONNXRT_VERSION_MISMATCH", ErrorCategory::kPermanent, false, std::nullopt};
constexpr OnnxErrorInfo kOpsetIncompatible{
    "CX_ERR_ONNX_OPSET_INCOMPATIBLE", ErrorCategory::kPermanent, false, std::nullopt};
constexpr OnnxErrorInfo kInferenceFailed{
    "CX_ERR_ONNX_INFERENCE_FAILED", ErrorCategory::kTransient, true, 200};

}  // namespace

const OnnxErrorInfo& GetOnnxErrorInfo(OnnxErrorCode code) {
    switch (code) {
        case OnnxErrorCode::kRuntimeVersionMismatch: return kRuntimeVersionMismatch;
        case OnnxErrorCode::kOpsetIncompatible:      return kOpsetIncompatible;
        case OnnxErrorCode::kInferenceFailed:        return kInferenceFailed;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInferenceFailed;
}

const char* OnnxErrorCodeString(OnnxErrorCode code) {
    return GetOnnxErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(OnnxErrorCode code) {
    // F22 §8.3 structured_data column, 1:1. Function-local statics → stable
    // references. `action` is included for the two startup errors because §8.3
    // carries an `action` field telling the Agent how to decide
    // (replace_so_or_rebuild / downgrade_model_or_upgrade_runtime).
    static const std::vector<std::string> kVersionMismatch{
        "current_version", "expected_major_version", "action"};
    static const std::vector<std::string> kOpset{
        "model_path", "model_opset", "supported_opset_range", "action"};
    static const std::vector<std::string> kInference{
        "onnx_error_code", "op_kernel", "last_input_shape"};

    switch (code) {
        case OnnxErrorCode::kRuntimeVersionMismatch: return kVersionMismatch;
        case OnnxErrorCode::kOpsetIncompatible:      return kOpset;
        case OnnxErrorCode::kInferenceFailed:        return kInference;
    }
    return kInference;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(OnnxErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeOnnxError(OnnxErrorCode code,
                                 nlohmann::json structured_data,
                                 const std::string& message) {
    const OnnxErrorInfo& info = GetOnnxErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode OnnxErrorToStatusCode(OnnxErrorCode code) {
    switch (code) {
        // Version mismatch + opset incompatible are permanent, caller-side
        // (wrong `.so` / wrong model for this build) — not a transient server
        // fault → kInvalidArgument. The rich CX_ERR_* identity + category are
        // preserved via the token + boundary MakeOnnxError().
        case OnnxErrorCode::kRuntimeVersionMismatch:
        case OnnxErrorCode::kOpsetIncompatible:
            return StatusCode::kInvalidArgument;
        // Inference failure is transient / retryable → kUnavailable.
        case OnnxErrorCode::kInferenceFailed:
            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status OnnxStatus(OnnxErrorCode code, const std::string& detail) {
    const char* cx = OnnxErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(OnnxErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::onnx
