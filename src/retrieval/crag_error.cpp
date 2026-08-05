#include "cortrix/retrieval/crag_error.h"

#include <utility>

namespace cortrix::retrieval {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so each
// returns a stable reference. The switches below are intentionally exhaustive:
// building with -Wall -Wextra (-Wswitch) turns "added a code without a row" into a
// warning, which the project treats as a build failure — the registry can't
// silently drift from the enum.
//
// retry_after_ms (§4.3 table):
//   CLASSIFIER_LOAD_FAILED → permanent (operator must fix model/config), null.
//   INFERENCE_FAILED       → transient, 200ms (matches the L3 retry back-off cap
//                            50/100/200 in §7.3).
//   THRESHOLD_INVALID      → permanent (bad NS crag_config), null.
//   FALLBACK_TRIGGERED     → transient; §4.3 lists "-" (unspecified) → null. This
//                            code is informational (the transparent all-Correct
//                            degrade already happened); there is no caller action,
//                            so no concrete back-off is advertised.
constexpr CragErrorInfo kClassifierLoadFailed{
    "CX_ERR_F37_CLASSIFIER_LOAD_FAILED", ErrorCategory::kPermanent, false, std::nullopt};
constexpr CragErrorInfo kInferenceFailed{
    "CX_ERR_F37_INFERENCE_FAILED", ErrorCategory::kTransient, true, 200};
constexpr CragErrorInfo kThresholdInvalid{
    "CX_ERR_F37_THRESHOLD_INVALID", ErrorCategory::kPermanent, false, std::nullopt};
constexpr CragErrorInfo kFallbackTriggered{
    "CX_ERR_F37_FALLBACK_TRIGGERED", ErrorCategory::kTransient, true, std::nullopt};

}  // namespace

const CragErrorInfo& GetCragErrorInfo(CragErrorCode code) {
    switch (code) {
        case CragErrorCode::kClassifierLoadFailed: return kClassifierLoadFailed;
        case CragErrorCode::kInferenceFailed:      return kInferenceFailed;
        case CragErrorCode::kThresholdInvalid:     return kThresholdInvalid;
        case CragErrorCode::kFallbackTriggered:    return kFallbackTriggered;
    }
    return kClassifierLoadFailed;  // unreachable for a valid enum value
}

const char* CragErrorCodeString(CragErrorCode code) {
    return GetCragErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(CragErrorCode code) {
    // §4.3 structured_data column, 1:1. Function-local statics → stable references.
    static const std::vector<std::string> kLoad{"model_path", "version"};
    static const std::vector<std::string> kInfer{"chunk_id", "retry_count"};
    static const std::vector<std::string> kThreshold{"invalid_field", "value"};
    static const std::vector<std::string> kFallback{"fallback_reason"};

    switch (code) {
        case CragErrorCode::kClassifierLoadFailed: return kLoad;
        case CragErrorCode::kInferenceFailed:      return kInfer;
        case CragErrorCode::kThresholdInvalid:     return kThreshold;
        case CragErrorCode::kFallbackTriggered:    return kFallback;
    }
    return kLoad;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(CragErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeCragError(CragErrorCode code,
                                 nlohmann::json structured_data,
                                 const std::string& message) {
    const CragErrorInfo& info = GetCragErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode CragErrorToStatusCode(CragErrorCode code) {
    switch (code) {
        // Classifier load / bad threshold are permanent, operator-side (bad
        // model / config) — not a transient server fault → kInvalidArgument. The
        // rich CX_ERR_* identity + category survive via the token + boundary
        // MakeCragError().
        case CragErrorCode::kClassifierLoadFailed:
        case CragErrorCode::kThresholdInvalid:
            return StatusCode::kInvalidArgument;
        // Inference failure / fallback are transient / retryable → kUnavailable.
        case CragErrorCode::kInferenceFailed:
        case CragErrorCode::kFallbackTriggered:
            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status CragStatus(CragErrorCode code, const std::string& detail) {
    const char* cx = CragErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(CragErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::retrieval
