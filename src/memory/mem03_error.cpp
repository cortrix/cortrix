#include "cortrix/memory/mem03_error.h"

#include <utility>

namespace cortrix::memory::transparency {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics
// so each returns a stable reference. The switch in GetMem03ErrorInfo is
// intentionally exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a
// code without a row" into a warning (treated as a build failure), so the registry
// can't silently drift from the enum.
//
// retry_after_ms follows §4.3.4.bis: INVALIDATE_FAILED (transient DB/oplog/trace
// fault) advises 5000ms; QUOTA (rate limit) advises 60000ms. The three non-retryable
// faults (MEMORY_NOT_FOUND / USER_MISMATCH / ALREADY_INVALIDATED) carry no hint.
constexpr Mem03ErrorInfo kMemoryNotFound
    {"CX_ERR_MEMORY_NOT_FOUND",    404, ErrorCategory::kPermanent,  false, std::nullopt};
constexpr Mem03ErrorInfo kUserMismatch
    {"CX_ERR_MEMORY_USER_MISMATCH",       403, ErrorCategory::kAuth,       false, std::nullopt};
constexpr Mem03ErrorInfo kAlreadyInvalidated
    {"CX_ERR_MEMORY_ALREADY_INVALIDATED", 410, ErrorCategory::kPermanent,  false, std::nullopt};
constexpr Mem03ErrorInfo kInvalidateFailed
    {"CX_ERR_MEMORY_INVALIDATE_FAILED",   500, ErrorCategory::kTransient,  true,  5000};
constexpr Mem03ErrorInfo kQuota
    {"CX_ERR_MEMORY_QUOTA",               429, ErrorCategory::kQuota,      true,  60000};

}  // namespace

const Mem03ErrorInfo& GetMem03ErrorInfo(Mem03ErrorCode code) {
    switch (code) {
        case Mem03ErrorCode::kMemoryNotFound:     return kMemoryNotFound;
        case Mem03ErrorCode::kUserMismatch:       return kUserMismatch;
        case Mem03ErrorCode::kAlreadyInvalidated: return kAlreadyInvalidated;
        case Mem03ErrorCode::kInvalidateFailed:   return kInvalidateFailed;
        case Mem03ErrorCode::kQuota:              return kQuota;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInvalidateFailed;
}

const char* Mem03ErrorCodeString(Mem03ErrorCode code) {
    return GetMem03ErrorInfo(code).cx_code;
}

int Mem03ErrorHttpStatus(Mem03ErrorCode code) {
    return GetMem03ErrorInfo(code).http_status;
}

const std::vector<std::string>& RequiredStructuredDataKeys(Mem03ErrorCode code) {
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
        case Mem03ErrorCode::kMemoryNotFound:     return kNotFoundKeys;
        case Mem03ErrorCode::kUserMismatch:       return kUserMismatchKeys;
        case Mem03ErrorCode::kAlreadyInvalidated: return kAlreadyInvalidatedKeys;
        case Mem03ErrorCode::kInvalidateFailed:   return kInvalidateFailedKeys;
        case Mem03ErrorCode::kQuota:              return kQuotaKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(Mem03ErrorCode code,
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

AgentFriendlyError MakeMem03Error(Mem03ErrorCode code,
                                  nlohmann::json structured_data,
                                  const std::string& message) {
    const Mem03ErrorInfo& info = GetMem03ErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode Mem03ErrorToStatusCode(Mem03ErrorCode code) {
    switch (code) {
        // memory_id absent / cross-user mask → kNotFound (the 404 surface).
        case Mem03ErrorCode::kMemoryNotFound:     return StatusCode::kNotFound;
        // caller ≠ owner → kPermissionDenied (the 403 surface).
        case Mem03ErrorCode::kUserMismatch:       return StatusCode::kPermissionDenied;
        // already invalidated (410 Gone) → kAlreadyExists is the closest coarse code
        // (the resource is in a terminal state); rich category=permanent + the
        // CX_ERR_ token + MakeMem03Error preserve the 410 semantics at the boundary.
        case Mem03ErrorCode::kAlreadyInvalidated: return StatusCode::kAlreadyExists;
        // low-level write fault → kInternal (server-side processing fault).
        case Mem03ErrorCode::kInvalidateFailed:   return StatusCode::kInternal;
        // rate limit (429) → kUnavailable (the request can be retried after backoff);
        // rich category=quota preserved via the CX_ERR_ token + MakeMem03Error.
        case Mem03ErrorCode::kQuota:              return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status Mem03Status(Mem03ErrorCode code, const std::string& detail) {
    const char* cx = Mem03ErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(Mem03ErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::memory::transparency
