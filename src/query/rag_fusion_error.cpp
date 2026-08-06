#include "cortrix/query/rag_fusion_error.h"

#include <utility>

namespace cortrix::query {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so each
// returns a stable reference. The switch in GetRagFusionErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without a
// row" into a warning (treated as a build failure), so the registry can't silently
// drift from the enum.
//
// retry_after_ms follows: DEGRADED / LLM_TIMEOUT → 5000ms; LLM_QUOTA → 60000ms;
// LLM_CIRCUIT_OPEN → 30000ms; INVALID_RESPONSE (permanent) / CONFIG_INVALID
// (startup) → null.
constexpr RagFusionErrorInfo kDegraded
    {"CX_WARN_RAG_FUSION_DEGRADED",        ErrorCategory::kTransient, true,  5000};
constexpr RagFusionErrorInfo kLlmTimeout
    {"CX_ERR_RAG_FUSION_LLM_TIMEOUT",      ErrorCategory::kTimeout,   true,  5000};
constexpr RagFusionErrorInfo kLlmQuota
    {"CX_ERR_RAG_FUSION_LLM_QUOTA",        ErrorCategory::kQuota,     true,  60000};
constexpr RagFusionErrorInfo kLlmCircuitOpen
    {"CX_ERR_RAG_FUSION_LLM_CIRCUIT_OPEN", ErrorCategory::kTransient, true,  30000};
constexpr RagFusionErrorInfo kInvalidResponse
    {"CX_ERR_RAG_FUSION_INVALID_RESPONSE", ErrorCategory::kPermanent, false, std::nullopt};
constexpr RagFusionErrorInfo kConfigInvalid
    {"CX_ERR_RAG_FUSION_CONFIG_INVALID",   ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const RagFusionErrorInfo& GetRagFusionErrorInfo(RagFusionErrorCode code) {
    switch (code) {
        case RagFusionErrorCode::kDegraded:        return kDegraded;
        case RagFusionErrorCode::kLlmTimeout:      return kLlmTimeout;
        case RagFusionErrorCode::kLlmQuota:        return kLlmQuota;
        case RagFusionErrorCode::kLlmCircuitOpen:  return kLlmCircuitOpen;
        case RagFusionErrorCode::kInvalidResponse: return kInvalidResponse;
        case RagFusionErrorCode::kConfigInvalid:   return kConfigInvalid;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInvalidResponse;
}

const char* RagFusionErrorCodeString(RagFusionErrorCode code) {
    return GetRagFusionErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(RagFusionErrorCode code) {
    // structured_data column. Function-local statics → stable refs.
    static const std::vector<std::string> kDegradedKeys
        {"degrade_reason", "llm_error", "original_query_used"};
    static const std::vector<std::string> kTimeoutKeys
        {"timeout_ms", "llm_endpoint"};
    static const std::vector<std::string> kQuotaKeys
        {"quota_type", "current_usage", "quota_limit"};
    static const std::vector<std::string> kCircuitKeys
        {"circuit_state", "next_retry_at"};
    static const std::vector<std::string> kInvalidKeys
        {"llm_response", "schema_validation_error"};
    static const std::vector<std::string> kConfigKeys
        {"config_field", "valid_range"};

    switch (code) {
        case RagFusionErrorCode::kDegraded:        return kDegradedKeys;
        case RagFusionErrorCode::kLlmTimeout:      return kTimeoutKeys;
        case RagFusionErrorCode::kLlmQuota:        return kQuotaKeys;
        case RagFusionErrorCode::kLlmCircuitOpen:  return kCircuitKeys;
        case RagFusionErrorCode::kInvalidResponse: return kInvalidKeys;
        case RagFusionErrorCode::kConfigInvalid:   return kConfigKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(RagFusionErrorCode code,
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

AgentFriendlyError MakeRagFusionError(RagFusionErrorCode code,
                                      nlohmann::json structured_data,
                                      const std::string& message) {
    const RagFusionErrorInfo& info = GetRagFusionErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode RagFusionErrorToStatusCode(RagFusionErrorCode code) {
    switch (code) {
        // timeouts / transient (degraded, circuit-open) → kUnavailable.
        case RagFusionErrorCode::kDegraded:
        case RagFusionErrorCode::kLlmTimeout:
        case RagFusionErrorCode::kLlmCircuitOpen:  return StatusCode::kUnavailable;
        // quota → kPermissionDenied (closest coarse code; rich category=quota
        // preserved via the CX_ERR_ token + boundary MakeRagFusionError).
        case RagFusionErrorCode::kLlmQuota:        return StatusCode::kPermissionDenied;
        // permanent LLM contract violation → kInternal (server-side, not retryable).
        case RagFusionErrorCode::kInvalidResponse: return StatusCode::kInternal;
        // bad config supplied → kInvalidArgument (startup / config-time).
        case RagFusionErrorCode::kConfigInvalid:   return StatusCode::kInvalidArgument;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status RagFusionStatus(RagFusionErrorCode code, const std::string& detail) {
    const char* cx = RagFusionErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(RagFusionErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::query
