#include "cortrix/spc/hype_error.h"

#include <utility>

namespace cortrix::spc {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Function-local statics → stable refs. The
// switches below are intentionally exhaustive: -Wswitch turns "added a code
// without a row" into a build failure, so the registry can't drift from the enum.
//
// retry_after_ms (§7 table): LLM_TIMEOUT 5000 / LLM_BUDGET_EXCEEDED 60000 are
// explicit; INVALID_OUTPUT + QUESTION_PARSE_FAILED are transient/retryable with
// no §7-stated interval → a modest 200ms (re-ask the LLM). SCHEMA_VERSION_MISMATCH
// + PARENT_NOT_FOUND are permanent (operator/data fix) → not retryable.
constexpr HypeErrorInfo kLlmTimeout{
    "CX_ERR_F38_LLM_TIMEOUT", ErrorCategory::kTimeout, true, 5000};
constexpr HypeErrorInfo kLlmInvalidOutput{
    "CX_ERR_F38_LLM_INVALID_OUTPUT", ErrorCategory::kTransient, true, 200};
constexpr HypeErrorInfo kLlmBudgetExceeded{
    "CX_ERR_F38_LLM_BUDGET_EXCEEDED", ErrorCategory::kQuota, true, 60000};
constexpr HypeErrorInfo kQuestionParseFailed{
    "CX_ERR_F38_QUESTION_PARSE_FAILED", ErrorCategory::kTransient, true, 200};
constexpr HypeErrorInfo kSchemaVersionMismatch{
    "CX_ERR_F38_SCHEMA_VERSION_MISMATCH", ErrorCategory::kPermanent, false, std::nullopt};
constexpr HypeErrorInfo kParentNotFound{
    "CX_ERR_F38_PARENT_NOT_FOUND", ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const HypeErrorInfo& GetHypeErrorInfo(HypeErrorCode code) {
    switch (code) {
        case HypeErrorCode::kLlmTimeout:            return kLlmTimeout;
        case HypeErrorCode::kLlmInvalidOutput:      return kLlmInvalidOutput;
        case HypeErrorCode::kLlmBudgetExceeded:     return kLlmBudgetExceeded;
        case HypeErrorCode::kQuestionParseFailed:   return kQuestionParseFailed;
        case HypeErrorCode::kSchemaVersionMismatch: return kSchemaVersionMismatch;
        case HypeErrorCode::kParentNotFound:        return kParentNotFound;
    }
    return kLlmTimeout;  // unreachable for a valid enum value
}

const char* HypeErrorCodeString(HypeErrorCode code) {
    return GetHypeErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(HypeErrorCode code) {
    // §7 structured_data column, 1:1. Function-local statics → stable references.
    static const std::vector<std::string> kTimeout{"chunk_id", "attempt_count",
                                                    "last_error_message"};
    static const std::vector<std::string> kInvalid{"chunk_id", "raw_output_preview",
                                                    "parse_error"};
    static const std::vector<std::string> kBudget{"chunk_id", "budget_remaining_usd",
                                                   "budget_reset_at"};
    static const std::vector<std::string> kParse{"chunk_id", "expected_count",
                                                  "actual_count"};
    static const std::vector<std::string> kSchema{"expected_version",
                                                   "actual_version"};
    static const std::vector<std::string> kParent{"parent_id", "store_error"};

    switch (code) {
        case HypeErrorCode::kLlmTimeout:            return kTimeout;
        case HypeErrorCode::kLlmInvalidOutput:      return kInvalid;
        case HypeErrorCode::kLlmBudgetExceeded:     return kBudget;
        case HypeErrorCode::kQuestionParseFailed:   return kParse;
        case HypeErrorCode::kSchemaVersionMismatch: return kSchema;
        case HypeErrorCode::kParentNotFound:        return kParent;
    }
    return kTimeout;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(HypeErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeHypeError(HypeErrorCode code, nlohmann::json structured_data,
                                 const std::string& message) {
    const HypeErrorInfo& info = GetHypeErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode HypeErrorToStatusCode(HypeErrorCode code) {
    switch (code) {
        // Schema mismatch = operator/config fault (permanent) → kInvalidArgument.
        case HypeErrorCode::kSchemaVersionMismatch:
            return StatusCode::kInvalidArgument;
        // Missing parent → kNotFound (matches the frozen ParentChunkStore contract).
        case HypeErrorCode::kParentNotFound:
            return StatusCode::kNotFound;
        // LLM timeout / invalid output / budget / parse are transient server-side
        // → kUnavailable (retryable, with retry_after_ms / quota semantics).
        case HypeErrorCode::kLlmTimeout:
        case HypeErrorCode::kLlmInvalidOutput:
        case HypeErrorCode::kLlmBudgetExceeded:
        case HypeErrorCode::kQuestionParseFailed:
            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status HypeStatus(HypeErrorCode code, const std::string& detail) {
    const char* cx = HypeErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(HypeErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::spc
