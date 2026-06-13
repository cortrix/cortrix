#include "cortrix/spc_enricher/enricher_error.h"

#include <utility>

namespace cortrix::spc {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (F03 §5.1 matrix). Defined as function-local
// statics so each returns a stable reference. The switches below are
// intentionally exhaustive: building with -Wall -Wextra (-Wswitch) turns "added
// a code without a row" into a warning, which the project treats as a build
// failure — the registry can't silently drift from the enum.
//
// retry_after_ms (§5.1 table): LLM_TIMEOUT → 1000; LLM_API → 2000; RATE_LIMIT →
// 60000 (default Retry-After header value, overridden per-call by the actual
// header). The matrix's "-1 = N/A" sentinel for non-retryable codes is expressed
// as std::nullopt (the AgentFriendlyError.retry_after_ms optional serializes to
// JSON null, AGENT_FRIENDLY §3.1) — same convention as reranker_error.h.
constexpr EnricherErrorInfo kInitFailed{
    "CX_ERR_ENRICHER_INIT_FAILED", ErrorCategory::kPermanent, false, std::nullopt};
constexpr EnricherErrorInfo kLlmTimeout{
    "CX_ERR_ENRICHER_LLM_TIMEOUT", ErrorCategory::kTimeout, true, 1000};
constexpr EnricherErrorInfo kLlmApi{
    "CX_ERR_ENRICHER_LLM_API", ErrorCategory::kTransient, true, 2000};
constexpr EnricherErrorInfo kParse{
    "CX_ERR_ENRICHER_PARSE", ErrorCategory::kPermanent, false, std::nullopt};
constexpr EnricherErrorInfo kBudget{
    "CX_ERR_ENRICHER_BUDGET", ErrorCategory::kQuota, false, std::nullopt};
constexpr EnricherErrorInfo kRateLimit{
    "CX_ERR_ENRICHER_RATE_LIMIT", ErrorCategory::kTransient, true, 60000};

}  // namespace

const EnricherErrorInfo& GetEnricherErrorInfo(EnricherErrorCode code) {
    switch (code) {
        case EnricherErrorCode::kInitFailed: return kInitFailed;
        case EnricherErrorCode::kLlmTimeout: return kLlmTimeout;
        case EnricherErrorCode::kLlmApi:     return kLlmApi;
        case EnricherErrorCode::kParse:      return kParse;
        case EnricherErrorCode::kBudget:     return kBudget;
        case EnricherErrorCode::kRateLimit:  return kRateLimit;
    }
    return kInitFailed;  // unreachable for a valid enum value
}

const char* EnricherErrorCodeString(EnricherErrorCode code) {
    return GetEnricherErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(EnricherErrorCode code) {
    // §5.1 structured_data column, 1:1. Function-local statics → stable refs.
    // (json_snippet / missing_chunk_indices are layer-specific extras the PARSE
    // call site adds; only the always-present keys are required here.)
    static const std::vector<std::string> kInit{"reason", "endpoint", "fallback"};
    static const std::vector<std::string> kTimeout{"model", "duration_ms", "task_timeout_ms"};
    static const std::vector<std::string> kApi{"model", "http_status", "retry_count"};
    static const std::vector<std::string> kParseKeys{"layer", "batch_size"};
    static const std::vector<std::string> kBudgetKeys{"budget_cap_usd", "current_cost_usd", "model"};
    static const std::vector<std::string> kRate{"model", "http_status", "retry_after_sec"};

    switch (code) {
        case EnricherErrorCode::kInitFailed: return kInit;
        case EnricherErrorCode::kLlmTimeout: return kTimeout;
        case EnricherErrorCode::kLlmApi:     return kApi;
        case EnricherErrorCode::kParse:      return kParseKeys;
        case EnricherErrorCode::kBudget:     return kBudgetKeys;
        case EnricherErrorCode::kRateLimit:  return kRate;
    }
    return kInit;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(EnricherErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeEnricherError(EnricherErrorCode code,
                                     nlohmann::json structured_data,
                                     const std::string& message) {
    const EnricherErrorInfo& info = GetEnricherErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode EnricherErrorToStatusCode(EnricherErrorCode code) {
    switch (code) {
        // Startup api_key/endpoint failures are permanent, caller-side (bad
        // config) — not a transient server fault → kInvalidArgument. The rich
        // CX_ERR_* identity + category survive via the token + boundary
        // MakeEnricherError().
        case EnricherErrorCode::kInitFailed:
            return StatusCode::kInvalidArgument;
        // Parse failures are permanent (malformed model output) → kInternal.
        case EnricherErrorCode::kParse:
            return StatusCode::kInternal;
        // Budget exhaustion is a quota condition → kPermissionDenied (the request
        // is refused until the operator raises budget_cap_usd / cycle resets).
        case EnricherErrorCode::kBudget:
            return StatusCode::kPermissionDenied;
        // Timeout / 5xx / rate-limit are transient / retryable → kUnavailable.
        case EnricherErrorCode::kLlmTimeout:
        case EnricherErrorCode::kLlmApi:
        case EnricherErrorCode::kRateLimit:
            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status EnricherStatus(EnricherErrorCode code, const std::string& detail) {
    const char* cx = EnricherErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(EnricherErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::spc
