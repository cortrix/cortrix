#include "cortrix/memory/memory_error.h"

#include <utility>

namespace cortrix::memory::transparency {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics
// so each returns a stable reference. The switch in GetMemoryErrorInfo is
// intentionally exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a
// code without a row" into a warning (treated as a build failure), so the registry
// can't silently drift from the enum.
//
// retry_after_ms follows §4.3.4.bis: INVALIDATE_FAILED (transient DB/oplog/trace
// fault) advises 5000ms; QUOTA (rate limit) advises 60000ms. The three non-retryable
// faults (MEMORY_NOT_FOUND / USER_MISMATCH / ALREADY_INVALIDATED) carry no hint.
constexpr MemoryErrorInfo kMemoryNotFound
    {"CX_ERR_MEMORY_NOT_FOUND",    404, ErrorCategory::kPermanent,  false, std::nullopt};
constexpr MemoryErrorInfo kUserMismatch
    {"CX_ERR_MEMORY_USER_MISMATCH",       403, ErrorCategory::kAuth,       false, std::nullopt};
constexpr MemoryErrorInfo kAlreadyInvalidated
    {"CX_ERR_MEMORY_ALREADY_INVALIDATED", 410, ErrorCategory::kPermanent,  false, std::nullopt};
constexpr MemoryErrorInfo kInvalidateFailed
    {"CX_ERR_MEMORY_INVALIDATE_FAILED",   500, ErrorCategory::kTransient,  true,  5000};
constexpr MemoryErrorInfo kQuota
    {"CX_ERR_MEMORY_QUOTA",               429, ErrorCategory::kQuota,      true,  60000};

}  // namespace

const MemoryErrorInfo& GetMemoryErrorInfo(MemoryErrorCode code) {
    switch (code) {
        case MemoryErrorCode::kMemoryNotFound:     return kMemoryNotFound;
        case MemoryErrorCode::kUserMismatch:       return kUserMismatch;
        case MemoryErrorCode::kAlreadyInvalidated: return kAlreadyInvalidated;
        case MemoryErrorCode::kInvalidateFailed:   return kInvalidateFailed;
        case MemoryErrorCode::kQuota:              return kQuota;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInvalidateFailed;
}

const char* MemoryErrorCodeString(MemoryErrorCode code) {
    return GetMemoryErrorInfo(code).cx_code;
}

int MemoryErrorHttpStatus(MemoryErrorCode code) {
    return GetMemoryErrorInfo(code).http_status;
}

const std::vector<std::string>& RequiredStructuredDataKeys(MemoryErrorCode code) {
    // §4.3.4.bis structured_data column + the data each fault needs to be actionable
    // by an Agent. Function-local statics → stable refs.
    static const std::vector<std::string> kNotFoundKeys
        {"memory_id"};
    static const std::vector<std::string> kUserMismatchKeys
        {"caller_user_id", "owner_user_id_masked"};
    static const std::vector<std::string> kAlreadyInvalidatedKeys
        {"memory_id", "revoked_at", "deleted_by_user_id"};
    static const std::vector<std::string> kInvalidateFailedKeys
        {"memory_id", "failure_stage"};
    static const std::vector<std::string> kQuotaKeys
        {"user_id", "namespace", "quota_used", "quota_limit", "window_seconds"};

    switch (code) {
        case MemoryErrorCode::kMemoryNotFound:     return kNotFoundKeys;
        case MemoryErrorCode::kUserMismatch:       return kUserMismatchKeys;
        case MemoryErrorCode::kAlreadyInvalidated: return kAlreadyInvalidatedKeys;
        case MemoryErrorCode::kInvalidateFailed:   return kInvalidateFailedKeys;
        case MemoryErrorCode::kQuota:              return kQuotaKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(MemoryErrorCode code,
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

AgentFriendlyError MakeMemoryError(MemoryErrorCode code,
                                  nlohmann::json structured_data,
                                  const std::string& message) {
    const MemoryErrorInfo& info = GetMemoryErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode MemoryErrorToStatusCode(MemoryErrorCode code) {
    switch (code) {
        // memory_id absent / cross-user mask → kNotFound (the 404 surface).
        case MemoryErrorCode::kMemoryNotFound:     return StatusCode::kNotFound;
        // caller ≠ owner → kPermissionDenied (the 403 surface).
        case MemoryErrorCode::kUserMismatch:       return StatusCode::kPermissionDenied;
        // already invalidated (410 Gone) → kAlreadyExists is the closest coarse code
        // (the resource is in a terminal state); rich category=permanent + the
        // CX_ERR_ token + MakeMemoryError preserve the 410 semantics at the boundary.
        case MemoryErrorCode::kAlreadyInvalidated: return StatusCode::kAlreadyExists;
        // low-level write fault → kInternal (server-side processing fault).
        case MemoryErrorCode::kInvalidateFailed:   return StatusCode::kInternal;
        // rate limit (429) → kUnavailable (the request can be retried after backoff);
        // rich category=quota preserved via the CX_ERR_ token + MakeMemoryError.
        case MemoryErrorCode::kQuota:              return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status MemoryStatus(MemoryErrorCode code, const std::string& detail) {
    const char* cx = MemoryErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(MemoryErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::memory::transparency
