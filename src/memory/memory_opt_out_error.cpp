#include "cortrix/memory/memory_opt_out_error.h"

#include <utility>

namespace cortrix::memory::immunity {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (the opt-out table). Defined as function-local
// statics so each returns a stable reference. The switch in GetMemoryOptOutErrorInfo is
// intentionally exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a
// code without a row" into a warning (treated as a build failure), so the registry
// can't silently drift from the enum.
//
// All 7 opt-out faults are permanent/auth client errors (bad session id, duplicate /
// missing opt-out state, missing admin scope, disabled feature, oversize metadata) —
// none is transient, so all carry retryable=false + retry_after_ms=null.
constexpr MemoryOptOutErrorInfo kSessionNotFound
    {"CX_ERR_MEMOPTOUT_SESSION_NOT_FOUND",  404, ErrorCategory::kPermanent, false, std::nullopt};
constexpr MemoryOptOutErrorInfo kAlreadyOptedOut
    {"CX_ERR_MEMOPTOUT_ALREADY_OPTED_OUT",  409, ErrorCategory::kPermanent, false, std::nullopt};
constexpr MemoryOptOutErrorInfo kNotOptedOut
    {"CX_ERR_MEMOPTOUT_NOT_OPTED_OUT",      409, ErrorCategory::kPermanent, false, std::nullopt};
constexpr MemoryOptOutErrorInfo kRevokeDenied
    {"CX_ERR_MEMOPTOUT_REVOKE_DENIED",      403, ErrorCategory::kAuth,      false, std::nullopt};
constexpr MemoryOptOutErrorInfo kOptOutDisabled
    {"CX_ERR_MEMOPTOUT_DISABLED",   503, ErrorCategory::kPermanent, false, std::nullopt};
constexpr MemoryOptOutErrorInfo kInvalidSessionId
    {"CX_ERR_MEMOPTOUT_INVALID_SESSION_ID", 422, ErrorCategory::kPermanent, false, std::nullopt};
constexpr MemoryOptOutErrorInfo kMetadataTooLarge
    {"CX_ERR_MEMOPTOUT_METADATA_TOO_LARGE", 422, ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const MemoryOptOutErrorInfo& GetMemoryOptOutErrorInfo(MemoryOptOutErrorCode code) {
    switch (code) {
        case MemoryOptOutErrorCode::kSessionNotFound:  return kSessionNotFound;
        case MemoryOptOutErrorCode::kAlreadyOptedOut:  return kAlreadyOptedOut;
        case MemoryOptOutErrorCode::kNotOptedOut:      return kNotOptedOut;
        case MemoryOptOutErrorCode::kRevokeDenied:     return kRevokeDenied;
        case MemoryOptOutErrorCode::kOptOutDisabled:   return kOptOutDisabled;
        case MemoryOptOutErrorCode::kInvalidSessionId: return kInvalidSessionId;
        case MemoryOptOutErrorCode::kMetadataTooLarge: return kMetadataTooLarge;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kSessionNotFound;
}

const char* MemoryOptOutErrorCodeString(MemoryOptOutErrorCode code) {
    return GetMemoryOptOutErrorInfo(code).cx_code;
}

int MemoryOptOutErrorHttpStatus(MemoryOptOutErrorCode code) {
    return GetMemoryOptOutErrorInfo(code).http_status;
}

const std::vector<std::string>& RequiredStructuredDataKeys(MemoryOptOutErrorCode code) {
    // ARCH §4.1.11 structured_data column + the data each fault needs to be actionable
    // by an Agent. Function-local statics → stable refs.
    static const std::vector<std::string> kSessionNotFoundKeys
        {"session_id"};
    static const std::vector<std::string> kAlreadyOptedOutKeys
        {"session_id", "opted_out_at"};
    static const std::vector<std::string> kNotOptedOutKeys
        {"session_id"};
    static const std::vector<std::string> kRevokeDeniedKeys
        {"required_role"};
    static const std::vector<std::string> kOptOutDisabledKeys
        {"config_source"};
    static const std::vector<std::string> kInvalidSessionIdKeys
        {"session_id", "expected_format"};
    static const std::vector<std::string> kMetadataTooLargeKeys
        {"metadata_bytes", "max_bytes"};

    switch (code) {
        case MemoryOptOutErrorCode::kSessionNotFound:  return kSessionNotFoundKeys;
        case MemoryOptOutErrorCode::kAlreadyOptedOut:  return kAlreadyOptedOutKeys;
        case MemoryOptOutErrorCode::kNotOptedOut:      return kNotOptedOutKeys;
        case MemoryOptOutErrorCode::kRevokeDenied:     return kRevokeDeniedKeys;
        case MemoryOptOutErrorCode::kOptOutDisabled:   return kOptOutDisabledKeys;
        case MemoryOptOutErrorCode::kInvalidSessionId: return kInvalidSessionIdKeys;
        case MemoryOptOutErrorCode::kMetadataTooLarge: return kMetadataTooLargeKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(MemoryOptOutErrorCode code,
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

AgentFriendlyError MakeMemoryOptOutError(MemoryOptOutErrorCode code,
                                  nlohmann::json structured_data,
                                  const std::string& message) {
    const MemoryOptOutErrorInfo& info = GetMemoryOptOutErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode MemoryOptOutErrorToStatusCode(MemoryOptOutErrorCode code) {
    switch (code) {
        // session row absent → kNotFound (the 404 surface).
        case MemoryOptOutErrorCode::kSessionNotFound:  return StatusCode::kNotFound;
        // already opted-out (409) → kAlreadyExists is the closest coarse code (the
        // resource is already in the requested state); rich category=permanent + the
        // CX_ERR_ token + MakeMemoryOptOutError preserve the 409 semantics at the boundary.
        case MemoryOptOutErrorCode::kAlreadyOptedOut:  return StatusCode::kAlreadyExists;
        // revoke on an active session (409 Conflict — there is nothing to revoke) →
        // kAlreadyExists (terminal-state conflict, same coarse bucket as above).
        case MemoryOptOutErrorCode::kNotOptedOut:      return StatusCode::kAlreadyExists;
        // missing admin scope → kPermissionDenied (the 403 surface).
        case MemoryOptOutErrorCode::kRevokeDenied:     return StatusCode::kPermissionDenied;
        // feature disabled by config (503) → kUnavailable (the request can succeed once
        // memory_opt_out.enabled flips); rich category=permanent preserved via the CX_ERR_ token.
        case MemoryOptOutErrorCode::kOptOutDisabled:   return StatusCode::kUnavailable;
        // malformed session id / oversize metadata (422) → kInvalidArgument (the
        // request itself is invalid; not retryable as-is).
        case MemoryOptOutErrorCode::kInvalidSessionId: return StatusCode::kInvalidArgument;
        case MemoryOptOutErrorCode::kMetadataTooLarge: return StatusCode::kInvalidArgument;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status MemoryOptOutStatus(MemoryOptOutErrorCode code, const std::string& detail) {
    const char* cx = MemoryOptOutErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(MemoryOptOutErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::memory::immunity
