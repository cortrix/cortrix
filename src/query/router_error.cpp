#include "cortrix/query/router_error.h"

#include <utility>

namespace cortrix::query {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (F39 §4.3). Defined as function-local statics so each
// returns a stable reference. The switch in GetRouterErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without a
// row" into a warning (treated as a build failure), so the registry can't silently
// drift from the enum.
//
// retry_after_ms follows §4.3: INFERENCE_FAILED → 100ms; FALLBACK_TRIGGERED →
// transient but no fixed delay (the degrade is already applied, "-" in the table);
// CLASSIFIER_LOAD_FAILED / FORCE_ROUTE_INVALID (permanent) → null.
constexpr RouterErrorInfo kClassifierLoadFailed
    {"CX_ERR_F39_CLASSIFIER_LOAD_FAILED", ErrorCategory::kPermanent, false, std::nullopt};
constexpr RouterErrorInfo kInferenceFailed
    {"CX_ERR_F39_INFERENCE_FAILED",       ErrorCategory::kTransient, true,  100};
constexpr RouterErrorInfo kForceRouteInvalid
    {"CX_ERR_F39_FORCE_ROUTE_INVALID",    ErrorCategory::kPermanent, false, std::nullopt};
constexpr RouterErrorInfo kFallbackTriggered
    {"CX_ERR_F39_FALLBACK_TRIGGERED",     ErrorCategory::kTransient, true,  std::nullopt};

}  // namespace

const RouterErrorInfo& GetRouterErrorInfo(RouterErrorCode code) {
    switch (code) {
        case RouterErrorCode::kClassifierLoadFailed: return kClassifierLoadFailed;
        case RouterErrorCode::kInferenceFailed:      return kInferenceFailed;
        case RouterErrorCode::kForceRouteInvalid:    return kForceRouteInvalid;
        case RouterErrorCode::kFallbackTriggered:    return kFallbackTriggered;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kClassifierLoadFailed;
}

const char* RouterErrorCodeString(RouterErrorCode code) {
    return GetRouterErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(RouterErrorCode code) {
    // §4.3 structured_data column. Function-local statics → stable refs.
    static const std::vector<std::string> kLoadKeys
        {"model_path", "version"};
    static const std::vector<std::string> kInferenceKeys
        {"query_preview", "retry_count"};
    static const std::vector<std::string> kForceRouteKeys
        {"invalid_route_value"};
    static const std::vector<std::string> kFallbackKeys
        {"fallback_reason"};

    switch (code) {
        case RouterErrorCode::kClassifierLoadFailed: return kLoadKeys;
        case RouterErrorCode::kInferenceFailed:      return kInferenceKeys;
        case RouterErrorCode::kForceRouteInvalid:    return kForceRouteKeys;
        case RouterErrorCode::kFallbackTriggered:    return kFallbackKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(RouterErrorCode code,
                               const nlohmann::json& structured_data) {
    const std::vector<std::string>& keys = RequiredStructuredDataKeys(code);
    if (!structured_data.is_object()) {
        return keys.empty();
    }
    for (const std::string& key : keys) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeRouterError(RouterErrorCode code,
                                   nlohmann::json structured_data,
                                   const std::string& message) {
    const RouterErrorInfo& info = GetRouterErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode RouterErrorToStatusCode(RouterErrorCode code) {
    switch (code) {
        // transient inference / degrade → kUnavailable.
        case RouterErrorCode::kInferenceFailed:
        case RouterErrorCode::kFallbackTriggered:    return StatusCode::kUnavailable;
        // bad route token supplied → kInvalidArgument (caller-side).
        case RouterErrorCode::kForceRouteInvalid:    return StatusCode::kInvalidArgument;
        // model load failure → kInternal (server-side, not retryable).
        case RouterErrorCode::kClassifierLoadFailed: return StatusCode::kInternal;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status RouterStatus(RouterErrorCode code, const std::string& detail) {
    const char* cx = RouterErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(RouterErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::query
