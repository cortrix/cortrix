#include "cortrix/chunker/chunker_agent_error.h"

#include <utility>

#include "cortrix/chunker/chunker_errors.h"  // chunk_errors::k* code strings (single source)

namespace cortrix::chunker {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per hard error (§ 5.1). All three are caller-side permanent
// (bad config / empty input / over-cap), retryable=false, retry_after_ms=null.
constexpr ChunkerErrorInfo kSizeInvalidInfo{
    chunk_errors::kSizeInvalid, ErrorCategory::kPermanent, false, std::nullopt};
constexpr ChunkerErrorInfo kEmptyDocInfo{
    chunk_errors::kEmptyDocument, ErrorCategory::kPermanent, false, std::nullopt};
constexpr ChunkerErrorInfo kOverflowInfo{
    chunk_errors::kOverflow, ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const ChunkerErrorInfo& GetChunkerErrorInfo(ChunkerErrorCode code) {
    switch (code) {
        case ChunkerErrorCode::kSizeInvalid:   return kSizeInvalidInfo;
        case ChunkerErrorCode::kEmptyDocument: return kEmptyDocInfo;
        case ChunkerErrorCode::kOverflow:      return kOverflowInfo;
    }
    return kSizeInvalidInfo;  // unreachable for a valid enum
}

const char* ChunkerErrorCodeString(ChunkerErrorCode code) {
    return GetChunkerErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(ChunkerErrorCode code) {
    // § 5.1 structured_data column, 1:1. Function-local statics → stable references.
    static const std::vector<std::string> kSize{"child_size", "max_seq_length"};
    static const std::vector<std::string> kEmpty{"doc_id", "total_pages", "failed_pages"};
    static const std::vector<std::string> kOverflow{
        "doc_id", "estimated_parent_count", "max_parents_per_doc", "fallback_attempted"};
    switch (code) {
        case ChunkerErrorCode::kSizeInvalid:   return kSize;
        case ChunkerErrorCode::kEmptyDocument: return kEmpty;
        case ChunkerErrorCode::kOverflow:      return kOverflow;
    }
    return kSize;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(ChunkerErrorCode code, const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeChunkerError(ChunkerErrorCode code,
                                    nlohmann::json structured_data,
                                    const std::string& message) {
    const ChunkerErrorInfo& info = GetChunkerErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode ChunkerErrorToStatusCode(ChunkerErrorCode code) {
    switch (code) {
        // All three are caller-side permanent (bad config / empty input / over the
        // cap) → kInvalidArgument; the rich CX_ERR_* identity + category survive via
        // the token prefix + boundary MakeChunkerError().
        case ChunkerErrorCode::kSizeInvalid:
        case ChunkerErrorCode::kEmptyDocument:
        case ChunkerErrorCode::kOverflow:
            return StatusCode::kInvalidArgument;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status ChunkerErrorStatus(ChunkerErrorCode code, const std::string& detail) {
    const char* cx = ChunkerErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx) : std::string(cx) + ": " + detail;
    return Status(ChunkerErrorToStatusCode(code), std::move(msg));
}

const char* ChunkerWarningCodeString(ChunkerWarningCode code) {
    switch (code) {
        case ChunkerWarningCode::kTruncated: return chunk_errors::kWarnTruncated;
        case ChunkerWarningCode::kFallback:  return chunk_errors::kWarnFallback;
    }
    return chunk_errors::kWarnFallback;  // unreachable
}

nlohmann::json MakeChunkWarning(ChunkerWarningCode code,
                                nlohmann::json structured_data,
                                const std::string& message) {
    const char* cx = ChunkerWarningCodeString(code);
    nlohmann::json w;
    w["code"] = cx;
    w["message"] = message.empty() ? std::string(cx) : message;
    w["structured_data"] = std::move(structured_data);
    return w;
}

}  // namespace cortrix::chunker
