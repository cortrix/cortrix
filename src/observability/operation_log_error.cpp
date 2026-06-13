#include "cortrix/observability/operation_log_error.h"

#include <utility>

namespace cortrix::observability {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (F18a §7.2). Defined as function-local statics so
// each returns a stable reference. The switch is intentionally exhaustive:
// building with -Wall -Wextra (-Wswitch) turns "added a code without a row" into
// a warning the project treats as a build failure — the registry can't silently
// drift from the enum.
constexpr OplogErrorInfo kInvalidFilter        {"CX_ERR_OPLOG_INVALID_FILTER",          ErrorCategory::kPermanent, false, std::nullopt};
constexpr OplogErrorInfo kInvalidTimestampRange{"CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE", ErrorCategory::kPermanent, false, std::nullopt};
constexpr OplogErrorInfo kPaginationOutOfRange {"CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE", ErrorCategory::kPermanent, false, std::nullopt};
constexpr OplogErrorInfo kUnauthorized         {"CX_ERR_OPLOG_UNAUTHORIZED",            ErrorCategory::kAuth,      false, std::nullopt};
constexpr OplogErrorInfo kCleanupRunning       {"CX_ERR_OPLOG_CLEANUP_RUNNING",         ErrorCategory::kTransient, true,  kOplogCleanupRetryBaseMs};
constexpr OplogErrorInfo kInternal             {"CX_ERR_OPLOG_INTERNAL",                ErrorCategory::kTransient, true,  std::nullopt};

}  // namespace

const OplogErrorInfo& GetOplogErrorInfo(OplogErrorCode code) {
    switch (code) {
        case OplogErrorCode::kInvalidFilter:         return kInvalidFilter;
        case OplogErrorCode::kInvalidTimestampRange: return kInvalidTimestampRange;
        case OplogErrorCode::kPaginationOutOfRange:  return kPaginationOutOfRange;
        case OplogErrorCode::kUnauthorized:          return kUnauthorized;
        case OplogErrorCode::kCleanupRunning:        return kCleanupRunning;
        case OplogErrorCode::kInternal:              return kInternal;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInternal;
}

const char* OplogErrorCodeString(OplogErrorCode code) {
    return GetOplogErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(OplogErrorCode code) {
    // §7.2 "structured_data required" column, 1:1. Function-local statics → stable
    // references.
    static const std::vector<std::string> kInvalidField{"invalid_field", "reason"};
    static const std::vector<std::string> kFromTo{"from", "to"};
    static const std::vector<std::string> kOffsetTotal{"offset", "total_count"};
    static const std::vector<std::string> kRequiredRole{"required_role"};
    static const std::vector<std::string> kRetryAt{"retry_at_ms"};
    static const std::vector<std::string> kErrorId{"error_id"};

    switch (code) {
        case OplogErrorCode::kInvalidFilter:         return kInvalidField;
        case OplogErrorCode::kInvalidTimestampRange: return kFromTo;
        case OplogErrorCode::kPaginationOutOfRange:  return kOffsetTotal;
        case OplogErrorCode::kUnauthorized:          return kRequiredRole;
        case OplogErrorCode::kCleanupRunning:        return kRetryAt;
        case OplogErrorCode::kInternal:              return kErrorId;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(OplogErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeOplogError(OplogErrorCode code,
                                  nlohmann::json structured_data,
                                  const std::string& message) {
    const OplogErrorInfo& info = GetOplogErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode OplogErrorToStatusCode(OplogErrorCode code) {
    switch (code) {
        // Filter / timestamp / pagination are caller-side permanent input faults.
        case OplogErrorCode::kInvalidFilter:
        case OplogErrorCode::kInvalidTimestampRange:
        case OplogErrorCode::kPaginationOutOfRange:
            return StatusCode::kInvalidArgument;
        // auth-category → kPermissionDenied (cross-user query without admin role).
        case OplogErrorCode::kUnauthorized:
            return StatusCode::kPermissionDenied;
        // Cleanup advisory-lock contention is transient → kUnavailable.
        case OplogErrorCode::kCleanupRunning:
            return StatusCode::kUnavailable;
        // Generic internal fault.
        case OplogErrorCode::kInternal:
            return StatusCode::kInternal;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status OplogStatus(OplogErrorCode code, const std::string& detail) {
    const char* cx = OplogErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(OplogErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::observability
