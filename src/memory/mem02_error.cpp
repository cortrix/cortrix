#include "cortrix/memory/mem02_error.h"

#include <utility>

namespace cortrix::memory {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (MEM02 §5.3). Defined as function-local statics so each
// returns a stable reference. The switch in GetMem02ErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without a
// row" into a warning (treated as a build failure), so the registry can't silently
// drift from the enum.
//
// retry_after_ms follows §5.3: the three transient/timeout LLM faults
// (EXTRACT_LLM_TIMEOUT / EXTRACT_INVALID_OUTPUT / CONTRADICTION_AMBIGUOUS) advise a
// 5000ms backoff (§5.2 example body). BUDGET_EXCEEDED (quota) + LLM_DISABLED
// (permanent) carry no retry hint.
constexpr Mem02ErrorInfo kExtractLlmTimeout
    {"CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT",      504, ErrorCategory::kTimeout,    true,  5000};
constexpr Mem02ErrorInfo kExtractInvalidOutput
    {"CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT",   500, ErrorCategory::kTransient,  true,  5000};
constexpr Mem02ErrorInfo kExtractBudgetExceeded
    {"CX_ERR_MEM02_EXTRACT_BUDGET_EXCEEDED",  429, ErrorCategory::kQuota,      false, std::nullopt};
constexpr Mem02ErrorInfo kContradictionAmbiguous
    {"CX_ERR_MEM02_CONTRADICTION_AMBIGUOUS",  500, ErrorCategory::kTransient,  true,  5000};
constexpr Mem02ErrorInfo kLlmDisabled
    {"CX_ERR_MEM02_LLM_DISABLED",             503, ErrorCategory::kPermanent,  false, std::nullopt};

}  // namespace

const Mem02ErrorInfo& GetMem02ErrorInfo(Mem02ErrorCode code) {
    switch (code) {
        case Mem02ErrorCode::kExtractLlmTimeout:      return kExtractLlmTimeout;
        case Mem02ErrorCode::kExtractInvalidOutput:   return kExtractInvalidOutput;
        case Mem02ErrorCode::kExtractBudgetExceeded:  return kExtractBudgetExceeded;
        case Mem02ErrorCode::kContradictionAmbiguous: return kContradictionAmbiguous;
        case Mem02ErrorCode::kLlmDisabled:            return kLlmDisabled;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kExtractInvalidOutput;
}

const char* Mem02ErrorCodeString(Mem02ErrorCode code) {
    return GetMem02ErrorInfo(code).cx_code;
}

int Mem02ErrorHttpStatus(Mem02ErrorCode code) {
    return GetMem02ErrorInfo(code).http_status;
}

const std::vector<std::string>& RequiredStructuredDataKeys(Mem02ErrorCode code) {
    // §5.2 structured_data example + the data each fault needs to be actionable by
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
        case Mem02ErrorCode::kExtractLlmTimeout:      return kTimeoutKeys;
        case Mem02ErrorCode::kExtractInvalidOutput:   return kInvalidOutputKeys;
        case Mem02ErrorCode::kExtractBudgetExceeded:  return kBudgetKeys;
        case Mem02ErrorCode::kContradictionAmbiguous: return kContradictionKeys;
        case Mem02ErrorCode::kLlmDisabled:            return kDisabledKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(Mem02ErrorCode code,
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

AgentFriendlyError MakeMem02Error(Mem02ErrorCode code,
                                  nlohmann::json structured_data,
                                  const std::string& message) {
    const Mem02ErrorInfo& info = GetMem02ErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode Mem02ErrorToStatusCode(Mem02ErrorCode code) {
    switch (code) {
        // LLM timeout → kUnavailable (transient, retryable upstream).
        case Mem02ErrorCode::kExtractLlmTimeout:      return StatusCode::kUnavailable;
        // bad LLM JSON / ambiguous judge → kInternal (server-side processing fault).
        case Mem02ErrorCode::kExtractInvalidOutput:   return StatusCode::kInternal;
        case Mem02ErrorCode::kContradictionAmbiguous: return StatusCode::kInternal;
        // budget exhausted → kPermissionDenied (closest coarse code; rich
        // category=quota preserved via the CX_ERR_ token + MakeMem02Error).
        case Mem02ErrorCode::kExtractBudgetExceeded:  return StatusCode::kPermissionDenied;
        // LLM disabled (NullEnricher) → kUnavailable (the capability is off).
        case Mem02ErrorCode::kLlmDisabled:            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status Mem02Status(Mem02ErrorCode code, const std::string& detail) {
    const char* cx = Mem02ErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(Mem02ErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::memory
