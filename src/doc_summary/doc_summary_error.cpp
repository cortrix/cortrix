#include "cortrix/doc_summary/doc_summary_error.h"

#include <utility>

namespace cortrix::doc_summary {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (F41 §7). Function-local statics → stable refs. The
// switches below are intentionally exhaustive: -Wswitch turns "added a code
// without a row" into a build failure, so the registry can't drift from the enum.
//
// retry_after_ms (§7 table): LLM_TIMEOUT 5000 / LLM_BUDGET_EXCEEDED 60000 are
// explicit. LLM_INVALID_OUTPUT / FALLBACK_FAILED / FTS5_FALLBACK_FAILED are
// transient/retryable with no §7-stated interval → a modest 200ms. DOC_TOO_LARGE
// + SCHEMA_VERSION_MISMATCH are permanent (data / operator fix) → not retryable.
constexpr DocSummaryErrorInfo kLlmTimeout{
    "CX_ERR_F41_LLM_TIMEOUT", ErrorCategory::kTimeout, true, 5000};
constexpr DocSummaryErrorInfo kLlmInvalidOutput{
    "CX_ERR_F41_LLM_INVALID_OUTPUT", ErrorCategory::kTransient, true, 200};
constexpr DocSummaryErrorInfo kLlmBudgetExceeded{
    "CX_ERR_F41_LLM_BUDGET_EXCEEDED", ErrorCategory::kQuota, true, 60000};
constexpr DocSummaryErrorInfo kDocTooLarge{
    "CX_ERR_F41_DOC_TOO_LARGE", ErrorCategory::kPermanent, false, std::nullopt};
constexpr DocSummaryErrorInfo kSchemaVersionMismatch{
    "CX_ERR_F41_SCHEMA_VERSION_MISMATCH", ErrorCategory::kPermanent, false, std::nullopt};
constexpr DocSummaryErrorInfo kFallbackFailed{
    "CX_ERR_F41_FALLBACK_FAILED", ErrorCategory::kTransient, true, 200};
constexpr DocSummaryErrorInfo kFts5FallbackFailed{
    "CX_ERR_F41_FTS5_FALLBACK_FAILED", ErrorCategory::kTransient, true, 200};

}  // namespace

const DocSummaryErrorInfo& GetDocSummaryErrorInfo(DocSummaryErrorCode code) {
    switch (code) {
        case DocSummaryErrorCode::kLlmTimeout:            return kLlmTimeout;
        case DocSummaryErrorCode::kLlmInvalidOutput:      return kLlmInvalidOutput;
        case DocSummaryErrorCode::kLlmBudgetExceeded:     return kLlmBudgetExceeded;
        case DocSummaryErrorCode::kDocTooLarge:           return kDocTooLarge;
        case DocSummaryErrorCode::kSchemaVersionMismatch: return kSchemaVersionMismatch;
        case DocSummaryErrorCode::kFallbackFailed:        return kFallbackFailed;
        case DocSummaryErrorCode::kFts5FallbackFailed:    return kFts5FallbackFailed;
    }
    return kLlmTimeout;  // unreachable for a valid enum value
}

const char* DocSummaryErrorCodeString(DocSummaryErrorCode code) {
    return GetDocSummaryErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(DocSummaryErrorCode code) {
    // §7 structured_data column, 1:1. Function-local statics → stable references.
    static const std::vector<std::string> kTimeout{"doc_id", "attempt_count",
                                                    "last_error_message"};
    static const std::vector<std::string> kInvalid{"doc_id", "raw_output_preview",
                                                    "parse_error"};
    static const std::vector<std::string> kBudget{"doc_id", "budget_remaining_usd",
                                                   "budget_reset_at"};
    static const std::vector<std::string> kTooLarge{"doc_id", "doc_size_bytes",
                                                     "max_allowed_bytes"};
    static const std::vector<std::string> kSchema{"expected_version",
                                                   "actual_version"};
    static const std::vector<std::string> kFallback{"doc_id", "fallback_type",
                                                     "error_message"};
    static const std::vector<std::string> kFts5{"doc_id", "fts5_query",
                                                 "error_message"};

    switch (code) {
        case DocSummaryErrorCode::kLlmTimeout:            return kTimeout;
        case DocSummaryErrorCode::kLlmInvalidOutput:      return kInvalid;
        case DocSummaryErrorCode::kLlmBudgetExceeded:     return kBudget;
        case DocSummaryErrorCode::kDocTooLarge:           return kTooLarge;
        case DocSummaryErrorCode::kSchemaVersionMismatch: return kSchema;
        case DocSummaryErrorCode::kFallbackFailed:        return kFallback;
        case DocSummaryErrorCode::kFts5FallbackFailed:    return kFts5;
    }
    return kTimeout;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(DocSummaryErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeDocSummaryError(DocSummaryErrorCode code,
                                       nlohmann::json structured_data,
                                       const std::string& message) {
    const DocSummaryErrorInfo& info = GetDocSummaryErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode DocSummaryErrorToStatusCode(DocSummaryErrorCode code) {
    switch (code) {
        // Doc-too-large = a rejected (oversized) input → kInvalidArgument.
        case DocSummaryErrorCode::kDocTooLarge:
            return StatusCode::kInvalidArgument;
        // Schema mismatch = operator/config fault (permanent) → kInvalidArgument.
        case DocSummaryErrorCode::kSchemaVersionMismatch:
            return StatusCode::kInvalidArgument;
        // LLM / fallback / FTS5 failures are transient server-side → kUnavailable.
        case DocSummaryErrorCode::kLlmTimeout:
        case DocSummaryErrorCode::kLlmInvalidOutput:
        case DocSummaryErrorCode::kLlmBudgetExceeded:
        case DocSummaryErrorCode::kFallbackFailed:
        case DocSummaryErrorCode::kFts5FallbackFailed:
            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status DocSummaryStatus(DocSummaryErrorCode code, const std::string& detail) {
    const char* cx = DocSummaryErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(DocSummaryErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::doc_summary
