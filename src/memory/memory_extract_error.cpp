#include "cortrix/memory/memory_extract_error.h"

#include <utility>

namespace cortrix::memory {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so each
// returns a stable reference. The switch in GetMemoryExtractErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without a
// row" into a warning (treated as a build failure), so the registry can't silently
// drift from the enum.
//
// retry_after_ms follows: the three transient/timeout LLM faults
// (EXTRACT_LLM_TIMEOUT / EXTRACT_INVALID_OUTPUT / CONTRADICTION_AMBIGUOUS) advise a
// 5000ms backoff (example body). BUDGET_EXCEEDED (quota) + LLM_DISABLED
// (permanent) carry no retry hint.
constexpr MemoryExtractErrorInfo kExtractLlmTimeout
    {"CX_ERR_MEMEXTRACT_LLM_TIMEOUT",      504, ErrorCategory::kTimeout,    true,  5000};
constexpr MemoryExtractErrorInfo kExtractInvalidOutput
    {"CX_ERR_MEMEXTRACT_INVALID_OUTPUT",   500, ErrorCategory::kTransient,  true,  5000};
constexpr MemoryExtractErrorInfo kExtractBudgetExceeded
    {"CX_ERR_MEMEXTRACT_BUDGET_EXCEEDED",  429, ErrorCategory::kQuota,      false, std::nullopt};
constexpr MemoryExtractErrorInfo kContradictionAmbiguous
    {"CX_ERR_MEMEXTRACT_CONTRADICTION_AMBIGUOUS",  500, ErrorCategory::kTransient,  true,  5000};
constexpr MemoryExtractErrorInfo kLlmDisabled
    {"CX_ERR_MEMEXTRACT_LLM_DISABLED",             503, ErrorCategory::kPermanent,  false, std::nullopt};

}  // namespace

const MemoryExtractErrorInfo& GetMemoryExtractErrorInfo(MemoryExtractErrorCode code) {
    switch (code) {
        case MemoryExtractErrorCode::kExtractLlmTimeout:      return kExtractLlmTimeout;
        case MemoryExtractErrorCode::kExtractInvalidOutput:   return kExtractInvalidOutput;
        case MemoryExtractErrorCode::kExtractBudgetExceeded:  return kExtractBudgetExceeded;
        case MemoryExtractErrorCode::kContradictionAmbiguous: return kContradictionAmbiguous;
        case MemoryExtractErrorCode::kLlmDisabled:            return kLlmDisabled;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kExtractInvalidOutput;
}

const char* MemoryExtractErrorCodeString(MemoryExtractErrorCode code) {
    return GetMemoryExtractErrorInfo(code).cx_code;
}

int MemoryExtractErrorHttpStatus(MemoryExtractErrorCode code) {
    return GetMemoryExtractErrorInfo(code).http_status;
}

const std::vector<std::string>& RequiredStructuredDataKeys(MemoryExtractErrorCode code) {
    // structured_data example + the data each fault needs to be actionable by
    // an Agent. Function-local statics → stable refs.
    static const std::vector<std::string> kTimeoutKeys
        {"interaction_id", "llm_model", "timeout_ms"};
    static const std::vector<std::string> kInvalidOutputKeys
        {"interaction_id", "llm_model"};
    static const std::vector<std::string> kBudgetKeys
        {"budget_cap_usd", "current_usage_usd"};
    static const std::vector<std::string> kContradictionKeys
        {"new_block_id", "old_block_id", "confidence"};
    static const std::vector<std::string> kDisabledKeys
        {"reason"};

    switch (code) {
        case MemoryExtractErrorCode::kExtractLlmTimeout:      return kTimeoutKeys;
        case MemoryExtractErrorCode::kExtractInvalidOutput:   return kInvalidOutputKeys;
        case MemoryExtractErrorCode::kExtractBudgetExceeded:  return kBudgetKeys;
        case MemoryExtractErrorCode::kContradictionAmbiguous: return kContradictionKeys;
        case MemoryExtractErrorCode::kLlmDisabled:            return kDisabledKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(MemoryExtractErrorCode code,
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

AgentFriendlyError MakeMemoryExtractError(MemoryExtractErrorCode code,
                                  nlohmann::json structured_data,
                                  const std::string& message) {
    const MemoryExtractErrorInfo& info = GetMemoryExtractErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode MemoryExtractErrorToStatusCode(MemoryExtractErrorCode code) {
    switch (code) {
        // LLM timeout → kUnavailable (transient, retryable upstream).
        case MemoryExtractErrorCode::kExtractLlmTimeout:      return StatusCode::kUnavailable;
        // bad LLM JSON / ambiguous judge → kInternal (server-side processing fault).
        case MemoryExtractErrorCode::kExtractInvalidOutput:   return StatusCode::kInternal;
        case MemoryExtractErrorCode::kContradictionAmbiguous: return StatusCode::kInternal;
        // budget exhausted → kPermissionDenied (closest coarse code; rich
        // category=quota preserved via the CX_ERR_ token + MakeMemoryExtractError).
        case MemoryExtractErrorCode::kExtractBudgetExceeded:  return StatusCode::kPermissionDenied;
        // LLM disabled (NullEnricher) → kUnavailable (the capability is off).
        case MemoryExtractErrorCode::kLlmDisabled:            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status MemoryExtractStatus(MemoryExtractErrorCode code, const std::string& detail) {
    const char* cx = MemoryExtractErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(MemoryExtractErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::memory
