#include "cortrix/server/batch_error.h"

#include <utility>

namespace cortrix::server {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (batch submit §2.4.1). Defined as function-local
// statics so each returns a stable reference. The switch in GetBatchErrorInfo is
// intentionally exhaustive: building with -Wall -Wextra (-Wswitch) turns "added
// a code without a row" into a warning (treated as a build failure), so the
// registry can't silently drift from the enum.
//
// http_status / category / retryable / retry_after_ms follow §2.4.1 exactly —
// all four batch-level faults are permanent envelope rejections, not retryable:
//   400 BATCH_SIZE_EXCEEDED     permanent  false  null
//   413 BATCH_PAYLOAD_TOO_LARGE permanent  false  null
//   400 BATCH_EMPTY             permanent  false  null
//   400 BATCH_DUPLICATE_DOC_ID  permanent  false  null
constexpr BatchErrorInfo kSizeExceeded
    {"CX_ERR_BATCH_SIZE_EXCEEDED",     400, ErrorCategory::kPermanent, false, std::nullopt};
constexpr BatchErrorInfo kPayloadTooLarge
    {"CX_ERR_BATCH_PAYLOAD_TOO_LARGE", 413, ErrorCategory::kPermanent, false, std::nullopt};
constexpr BatchErrorInfo kEmpty
    {"CX_ERR_BATCH_EMPTY",             400, ErrorCategory::kPermanent, false, std::nullopt};
constexpr BatchErrorInfo kDuplicateDocId
    {"CX_ERR_BATCH_DUPLICATE_DOC_ID",  400, ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const BatchErrorInfo& GetBatchErrorInfo(BatchErrorCode code) {
    switch (code) {
        case BatchErrorCode::kSizeExceeded:    return kSizeExceeded;
        case BatchErrorCode::kPayloadTooLarge: return kPayloadTooLarge;
        case BatchErrorCode::kEmpty:           return kEmpty;
        case BatchErrorCode::kDuplicateDocId:  return kDuplicateDocId;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kSizeExceeded;
}

const char* BatchErrorCodeString(BatchErrorCode code) {
    return GetBatchErrorInfo(code).cx_code;
}

int BatchErrorHttpStatus(BatchErrorCode code) {
    return GetBatchErrorInfo(code).http_status;
}

const std::vector<std::string>& BatchRequiredStructuredDataKeys(BatchErrorCode code) {
    // §2.4.1 trigger scenarios → the structured_data an Agent needs to react
    // (GEN-Agent #5). Function-local statics → stable refs.
    static const std::vector<std::string> kSizeExceededKeys
        {"max_size", "actual_size"};
    static const std::vector<std::string> kPayloadTooLargeKeys
        {"max_bytes", "actual_bytes"};
    static const std::vector<std::string> kEmptyKeys{};  // empty array → no extra context
    static const std::vector<std::string> kDuplicateKeys
        {"duplicate_doc_ids"};

    switch (code) {
        case BatchErrorCode::kSizeExceeded:    return kSizeExceededKeys;
        case BatchErrorCode::kPayloadTooLarge: return kPayloadTooLargeKeys;
        case BatchErrorCode::kEmpty:           return kEmptyKeys;
        case BatchErrorCode::kDuplicateDocId:  return kDuplicateKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool BatchHasRequiredStructuredData(BatchErrorCode code,
                                    const nlohmann::json& structured_data) {
    const std::vector<std::string>& keys = BatchRequiredStructuredDataKeys(code);
    if (!structured_data.is_object()) {
        return keys.empty();
    }
    for (const std::string& key : keys) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeBatchError(BatchErrorCode code,
                                  nlohmann::json structured_data,
                                  const std::string& message) {
    const BatchErrorInfo& info = GetBatchErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

}  // namespace cortrix::server
