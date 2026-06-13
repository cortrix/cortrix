#include "cortrix/async/f42_error.h"

#include <utility>

namespace cortrix::async {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (F42 §6.2). Defined as function-local statics so
// each returns a stable reference. The switch in GetF42ErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without
// a row" into a warning (treated as a build failure), so the registry can't
// silently drift from the enum.
//
// http_status / category / retryable / retry_after_ms follow §6.2 exactly:
//   400 INVALID_REQUEST            permanent  false  null
//   403 MAX_PAGES_EXCEEDED         permanent  false  null
//   404 TASK_NOT_FOUND             permanent  false  null
//   408 TASK_TIMEOUT               timeout    true   60000
//   409 DOC_PROCESSING_IN_PROGRESS transient  true   30000
//   423 TASK_CANCELLING            permanent  false  null
//   500 PARSE_FAILED               permanent  false  null
//   500 STORAGE_FAILED             permanent  false  null
//   500 WORKER_BUSY                transient  true   10000
//   500 ZOMBIE_TASK_CLEANUP        permanent  false  null
//   503 SERVICE_UNAVAILABLE        transient  true   10000
constexpr F42ErrorInfo kInvalidRequest
    {"CX_ERR_INVALID_REQUEST",             400, ErrorCategory::kPermanent, false, std::nullopt};
constexpr F42ErrorInfo kMaxPagesExceeded
    {"CX_ERR_MAX_PAGES_EXCEEDED",          403, ErrorCategory::kPermanent, false, std::nullopt};
constexpr F42ErrorInfo kTaskNotFound
    {"CX_ERR_TASK_NOT_FOUND",              404, ErrorCategory::kPermanent, false, std::nullopt};
constexpr F42ErrorInfo kTaskTimeout
    {"CX_ERR_TASK_TIMEOUT",                408, ErrorCategory::kTimeout,   true,  60000};
constexpr F42ErrorInfo kDocProcessingInProgress
    {"CX_ERR_DOC_PROCESSING_IN_PROGRESS",  409, ErrorCategory::kTransient, true,  30000};
constexpr F42ErrorInfo kTaskCancelling
    {"CX_ERR_TASK_CANCELLING",             423, ErrorCategory::kPermanent, false, std::nullopt};
constexpr F42ErrorInfo kParseFailed
    {"CX_ERR_PARSE_FAILED",                500, ErrorCategory::kPermanent, false, std::nullopt};
constexpr F42ErrorInfo kStorageFailed
    {"CX_ERR_STORAGE_FAILED",              500, ErrorCategory::kPermanent, false, std::nullopt};
constexpr F42ErrorInfo kWorkerBusy
    {"CX_ERR_WORKER_BUSY",                 500, ErrorCategory::kTransient, true,  10000};
constexpr F42ErrorInfo kZombieTaskCleanup
    {"CX_ERR_ZOMBIE_TASK_CLEANUP",         500, ErrorCategory::kPermanent, false, std::nullopt};
constexpr F42ErrorInfo kServiceUnavailable
    {"CX_ERR_SERVICE_UNAVAILABLE",         503, ErrorCategory::kTransient, true,  10000};

}  // namespace

const F42ErrorInfo& GetF42ErrorInfo(F42ErrorCode code) {
    switch (code) {
        case F42ErrorCode::kInvalidRequest:          return kInvalidRequest;
        case F42ErrorCode::kMaxPagesExceeded:        return kMaxPagesExceeded;
        case F42ErrorCode::kTaskNotFound:            return kTaskNotFound;
        case F42ErrorCode::kTaskTimeout:             return kTaskTimeout;
        case F42ErrorCode::kDocProcessingInProgress: return kDocProcessingInProgress;
        case F42ErrorCode::kTaskCancelling:          return kTaskCancelling;
        case F42ErrorCode::kParseFailed:             return kParseFailed;
        case F42ErrorCode::kStorageFailed:           return kStorageFailed;
        case F42ErrorCode::kWorkerBusy:              return kWorkerBusy;
        case F42ErrorCode::kZombieTaskCleanup:       return kZombieTaskCleanup;
        case F42ErrorCode::kServiceUnavailable:      return kServiceUnavailable;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInvalidRequest;
}

const char* F42ErrorCodeString(F42ErrorCode code) {
    return GetF42ErrorInfo(code).cx_code;
}

int F42ErrorHttpStatus(F42ErrorCode code) {
    return GetF42ErrorInfo(code).http_status;
}

const std::vector<std::string>& RequiredStructuredDataKeys(F42ErrorCode code) {
    // §6.2 structured_data column. Function-local statics → stable refs.
    static const std::vector<std::string> kInvalidRequestKeys
        {"rules_violated"};
    static const std::vector<std::string> kMaxPagesKeys
        {"max_pages", "actual_pages", "edition"};
    static const std::vector<std::string> kEmptyKeys{};  // §6.2: TASK_NOT_FOUND → {}
    static const std::vector<std::string> kTimeoutKeys
        {"task_id", "timeout_seconds", "last_phase"};
    static const std::vector<std::string> kDocInProgressKeys
        {"existing_task_id", "doc_id"};
    static const std::vector<std::string> kCancellingKeys
        {"task_id", "current_status"};
    static const std::vector<std::string> kParseFailedKeys
        {"task_id", "page_number", "parser_error"};
    static const std::vector<std::string> kStorageFailedKeys
        {"task_id", "filepath", "error"};
    static const std::vector<std::string> kWorkerBusyKeys
        {"active_tasks", "max_workers"};
    static const std::vector<std::string> kZombieKeys
        {"task_id", "last_alive_at"};
    static const std::vector<std::string> kServiceUnavailableKeys
        {"component"};

    switch (code) {
        case F42ErrorCode::kInvalidRequest:          return kInvalidRequestKeys;
        case F42ErrorCode::kMaxPagesExceeded:        return kMaxPagesKeys;
        case F42ErrorCode::kTaskNotFound:            return kEmptyKeys;
        case F42ErrorCode::kTaskTimeout:             return kTimeoutKeys;
        case F42ErrorCode::kDocProcessingInProgress: return kDocInProgressKeys;
        case F42ErrorCode::kTaskCancelling:          return kCancellingKeys;
        case F42ErrorCode::kParseFailed:             return kParseFailedKeys;
        case F42ErrorCode::kStorageFailed:           return kStorageFailedKeys;
        case F42ErrorCode::kWorkerBusy:              return kWorkerBusyKeys;
        case F42ErrorCode::kZombieTaskCleanup:       return kZombieKeys;
        case F42ErrorCode::kServiceUnavailable:      return kServiceUnavailableKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(F42ErrorCode code,
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

AgentFriendlyError MakeF42Error(F42ErrorCode code,
                                nlohmann::json structured_data,
                                const std::string& message) {
    const F42ErrorInfo& info = GetF42ErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode F42ErrorToStatusCode(F42ErrorCode code) {
    switch (code) {
        // bad request / page-cap violation → kInvalidArgument (client-side).
        case F42ErrorCode::kInvalidRequest:
        case F42ErrorCode::kMaxPagesExceeded:        return StatusCode::kInvalidArgument;
        case F42ErrorCode::kTaskNotFound:            return StatusCode::kNotFound;
        // 30-min timeout → kUnavailable (transient, retryable).
        case F42ErrorCode::kTaskTimeout:             return StatusCode::kUnavailable;
        // concurrent same-doc write in flight → kAlreadyExists (closest coarse code).
        case F42ErrorCode::kDocProcessingInProgress: return StatusCode::kAlreadyExists;
        // cancel-in-progress repeat → kAlreadyExists (the cancel is already underway).
        case F42ErrorCode::kTaskCancelling:          return StatusCode::kAlreadyExists;
        // parse / storage / zombie → kInternal (server-side, not retryable).
        case F42ErrorCode::kParseFailed:
        case F42ErrorCode::kStorageFailed:
        case F42ErrorCode::kZombieTaskCleanup:       return StatusCode::kInternal;
        // worker saturation / dependency down → kUnavailable (transient, retryable).
        case F42ErrorCode::kWorkerBusy:
        case F42ErrorCode::kServiceUnavailable:      return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status F42Status(F42ErrorCode code, const std::string& detail) {
    const char* cx = F42ErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(F42ErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::async
