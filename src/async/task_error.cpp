#include "cortrix/async/task_error.h"

#include <utility>

namespace cortrix::async {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so
// each returns a stable reference. The switch in GetTaskErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without
// a row" into a warning (treated as a build failure), so the registry can't
// silently drift from the enum.
//
// http_status / category / retryable / retry_after_ms follow exactly:
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
constexpr TaskErrorInfo kInvalidRequest
    {"CX_ERR_INVALID_REQUEST",             400, ErrorCategory::kPermanent, false, std::nullopt};
constexpr TaskErrorInfo kMaxPagesExceeded
    {"CX_ERR_MAX_PAGES_EXCEEDED",          403, ErrorCategory::kPermanent, false, std::nullopt};
constexpr TaskErrorInfo kTaskNotFound
    {"CX_ERR_TASK_NOT_FOUND",              404, ErrorCategory::kPermanent, false, std::nullopt};
constexpr TaskErrorInfo kTaskTimeout
    {"CX_ERR_TASK_TIMEOUT",                408, ErrorCategory::kTimeout,   true,  60000};
constexpr TaskErrorInfo kDocProcessingInProgress
    {"CX_ERR_DOC_PROCESSING_IN_PROGRESS",  409, ErrorCategory::kTransient, true,  30000};
constexpr TaskErrorInfo kTaskCancelling
    {"CX_ERR_TASK_CANCELLING",             423, ErrorCategory::kPermanent, false, std::nullopt};
constexpr TaskErrorInfo kParseFailed
    {"CX_ERR_PARSE_FAILED",                500, ErrorCategory::kPermanent, false, std::nullopt};
constexpr TaskErrorInfo kStorageFailed
    {"CX_ERR_STORAGE_FAILED",              500, ErrorCategory::kPermanent, false, std::nullopt};
constexpr TaskErrorInfo kWorkerBusy
    {"CX_ERR_WORKER_BUSY",                 500, ErrorCategory::kTransient, true,  10000};
constexpr TaskErrorInfo kZombieTaskCleanup
    {"CX_ERR_ZOMBIE_TASK_CLEANUP",         500, ErrorCategory::kPermanent, false, std::nullopt};
constexpr TaskErrorInfo kServiceUnavailable
    {"CX_ERR_SERVICE_UNAVAILABLE",         503, ErrorCategory::kTransient, true,  10000};

}  // namespace

const TaskErrorInfo& GetTaskErrorInfo(TaskErrorCode code) {
    switch (code) {
        case TaskErrorCode::kInvalidRequest:          return kInvalidRequest;
        case TaskErrorCode::kMaxPagesExceeded:        return kMaxPagesExceeded;
        case TaskErrorCode::kTaskNotFound:            return kTaskNotFound;
        case TaskErrorCode::kTaskTimeout:             return kTaskTimeout;
        case TaskErrorCode::kDocProcessingInProgress: return kDocProcessingInProgress;
        case TaskErrorCode::kTaskCancelling:          return kTaskCancelling;
        case TaskErrorCode::kParseFailed:             return kParseFailed;
        case TaskErrorCode::kStorageFailed:           return kStorageFailed;
        case TaskErrorCode::kWorkerBusy:              return kWorkerBusy;
        case TaskErrorCode::kZombieTaskCleanup:       return kZombieTaskCleanup;
        case TaskErrorCode::kServiceUnavailable:      return kServiceUnavailable;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInvalidRequest;
}

const char* TaskErrorCodeString(TaskErrorCode code) {
    return GetTaskErrorInfo(code).cx_code;
}

int TaskErrorHttpStatus(TaskErrorCode code) {
    return GetTaskErrorInfo(code).http_status;
}

const std::vector<std::string>& RequiredStructuredDataKeys(TaskErrorCode code) {
    // structured_data column. Function-local statics → stable refs.
    static const std::vector<std::string> kInvalidRequestKeys
        {"rules_violated"};
    static const std::vector<std::string> kMaxPagesKeys
        {"max_pages", "actual_pages", "edition"};
    static const std::vector<std::string> kEmptyKeys{};  //: TASK_NOT_FOUND → {}
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
        case TaskErrorCode::kInvalidRequest:          return kInvalidRequestKeys;
        case TaskErrorCode::kMaxPagesExceeded:        return kMaxPagesKeys;
        case TaskErrorCode::kTaskNotFound:            return kEmptyKeys;
        case TaskErrorCode::kTaskTimeout:             return kTimeoutKeys;
        case TaskErrorCode::kDocProcessingInProgress: return kDocInProgressKeys;
        case TaskErrorCode::kTaskCancelling:          return kCancellingKeys;
        case TaskErrorCode::kParseFailed:             return kParseFailedKeys;
        case TaskErrorCode::kStorageFailed:           return kStorageFailedKeys;
        case TaskErrorCode::kWorkerBusy:              return kWorkerBusyKeys;
        case TaskErrorCode::kZombieTaskCleanup:       return kZombieKeys;
        case TaskErrorCode::kServiceUnavailable:      return kServiceUnavailableKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(TaskErrorCode code,
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

AgentFriendlyError MakeTaskError(TaskErrorCode code,
                                nlohmann::json structured_data,
                                const std::string& message) {
    const TaskErrorInfo& info = GetTaskErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode TaskErrorToStatusCode(TaskErrorCode code) {
    switch (code) {
        // bad request / page-cap violation → kInvalidArgument (client-side).
        case TaskErrorCode::kInvalidRequest:
        case TaskErrorCode::kMaxPagesExceeded:        return StatusCode::kInvalidArgument;
        case TaskErrorCode::kTaskNotFound:            return StatusCode::kNotFound;
        // 30-min timeout → kUnavailable (transient, retryable).
        case TaskErrorCode::kTaskTimeout:             return StatusCode::kUnavailable;
        // concurrent same-doc write in flight → kAlreadyExists (closest coarse code).
        case TaskErrorCode::kDocProcessingInProgress: return StatusCode::kAlreadyExists;
        // cancel-in-progress repeat → kAlreadyExists (the cancel is already underway).
        case TaskErrorCode::kTaskCancelling:          return StatusCode::kAlreadyExists;
        // parse / storage / zombie → kInternal (server-side, not retryable).
        case TaskErrorCode::kParseFailed:
        case TaskErrorCode::kStorageFailed:
        case TaskErrorCode::kZombieTaskCleanup:       return StatusCode::kInternal;
        // worker saturation / dependency down → kUnavailable (transient, retryable).
        case TaskErrorCode::kWorkerBusy:
        case TaskErrorCode::kServiceUnavailable:      return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status TaskStatus(TaskErrorCode code, const std::string& detail) {
    const char* cx = TaskErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(TaskErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::async
