#include "cortrix/spc/contextual_error.h"

#include <utility>

namespace cortrix::spc {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Function-local statics → stable refs. The
// switches below are intentionally exhaustive: -Wswitch turns "added a code
// without a row" into a build failure, so the registry can't drift from the enum.
//
// retry_after_ms (§8 table): LLM_FAILED 1000 / EMBEDDING_FAILED 500 are explicit
// transient retries. BUDGET_EXCEEDED is a quota wall (no retry) and
// PROMPT_INJECTION / STARTUP_NO_LLM are permanent (defense / operator fix) → not
// retryable, no interval.
constexpr ContextualErrorInfo kLlmFailed{
    "CX_ERR_F35_LLM_FAILED", ErrorCategory::kTransient, true, 1000};
constexpr ContextualErrorInfo kBudgetExceeded{
    "CX_ERR_F35_BUDGET_EXCEEDED", ErrorCategory::kQuota, false, std::nullopt};
constexpr ContextualErrorInfo kPromptInjection{
    "CX_ERR_F35_PROMPT_INJECTION", ErrorCategory::kPermanent, false, std::nullopt};
constexpr ContextualErrorInfo kEmbeddingFailed{
    "CX_ERR_F35_EMBEDDING_FAILED", ErrorCategory::kTransient, true, 500};
constexpr ContextualErrorInfo kStartupNoLlm{
    "CX_ERR_F35_STARTUP_NO_LLM", ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const ContextualErrorInfo& GetContextualErrorInfo(ContextualErrorCode code) {
    switch (code) {
        case ContextualErrorCode::kLlmFailed:       return kLlmFailed;
        case ContextualErrorCode::kBudgetExceeded:  return kBudgetExceeded;
        case ContextualErrorCode::kPromptInjection: return kPromptInjection;
        case ContextualErrorCode::kEmbeddingFailed: return kEmbeddingFailed;
        case ContextualErrorCode::kStartupNoLlm:    return kStartupNoLlm;
    }
    return kLlmFailed;  // unreachable for a valid enum value
}

const char* ContextualErrorCodeString(ContextualErrorCode code) {
    return GetContextualErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(ContextualErrorCode code) {
    // §8 structured_data column, 1:1. Function-local statics → stable references.
    static const std::vector<std::string> kFailed{"chunk_id", "retry_count"};
    static const std::vector<std::string> kBudget{"ns_id", "monthly_quota", "used"};
    static const std::vector<std::string> kInjection{"chunk_id", "output_length"};
    static const std::vector<std::string> kEmbedding{"chunk_id"};
    static const std::vector<std::string> kStartup{"enricher_chain"};

    switch (code) {
        case ContextualErrorCode::kLlmFailed:       return kFailed;
        case ContextualErrorCode::kBudgetExceeded:  return kBudget;
        case ContextualErrorCode::kPromptInjection: return kInjection;
        case ContextualErrorCode::kEmbeddingFailed: return kEmbedding;
        case ContextualErrorCode::kStartupNoLlm:    return kStartup;
    }
    return kFailed;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(ContextualErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeContextualError(ContextualErrorCode code,
                                       nlohmann::json structured_data,
                                       const std::string& message) {
    const ContextualErrorInfo& info = GetContextualErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode ContextualErrorToStatusCode(ContextualErrorCode code) {
    switch (code) {
        // Prompt injection = a rejected (malformed) LLM output → kInvalidArgument
        // (permanent, an operator/data condition the Agent must not blindly retry).
        case ContextualErrorCode::kPromptInjection:
            return StatusCode::kInvalidArgument;
        // Budget exhausted → permission-style quota wall.
        case ContextualErrorCode::kBudgetExceeded:
            return StatusCode::kPermissionDenied;
        // LLM transient / embedding transient / startup-no-LLM are server-side
        // unavailability → kUnavailable (LLM_FAILED + EMBEDDING_FAILED retryable;
        // STARTUP_NO_LLM is a transparent degrade the caller maps to L2 skip).
        case ContextualErrorCode::kLlmFailed:
        case ContextualErrorCode::kEmbeddingFailed:
        case ContextualErrorCode::kStartupNoLlm:
            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status ContextualStatus(ContextualErrorCode code, const std::string& detail) {
    const char* cx = ContextualErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(ContextualErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::spc
