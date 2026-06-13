#include "cortrix/memory/mem04_error.h"

#include <utility>

namespace cortrix::memory::immunity {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (ARCH §4.1.11 MEM04 table). Defined as function-local
// statics so each returns a stable reference. The switch in GetMem04ErrorInfo is
// intentionally exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a
// code without a row" into a warning (treated as a build failure), so the registry
// can't silently drift from the enum.
//
// All 7 MEM04 faults are permanent/auth client errors (bad session id, duplicate /
// missing opt-out state, missing admin scope, disabled feature, oversize metadata) —
// none is transient, so all carry retryable=false + retry_after_ms=null.
constexpr Mem04ErrorInfo kSessionNotFound
    {"CX_ERR_MEM04_SESSION_NOT_FOUND",  404, ErrorCategory::kPermanent, false, std::nullopt};
constexpr Mem04ErrorInfo kAlreadyOptedOut
    {"CX_ERR_MEM04_ALREADY_OPTED_OUT",  409, ErrorCategory::kPermanent, false, std::nullopt};
constexpr Mem04ErrorInfo kNotOptedOut
    {"CX_ERR_MEM04_NOT_OPTED_OUT",      409, ErrorCategory::kPermanent, false, std::nullopt};
constexpr Mem04ErrorInfo kRevokeDenied
    {"CX_ERR_MEM04_REVOKE_DENIED",      403, ErrorCategory::kAuth,      false, std::nullopt};
constexpr Mem04ErrorInfo kOptOutDisabled
    {"CX_ERR_MEM04_OPT_OUT_DISABLED",   503, ErrorCategory::kPermanent, false, std::nullopt};
constexpr Mem04ErrorInfo kInvalidSessionId
    {"CX_ERR_MEM04_INVALID_SESSION_ID", 422, ErrorCategory::kPermanent, false, std::nullopt};
constexpr Mem04ErrorInfo kMetadataTooLarge
    {"CX_ERR_MEM04_METADATA_TOO_LARGE", 422, ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const Mem04ErrorInfo& GetMem04ErrorInfo(Mem04ErrorCode code) {
    switch (code) {
        case Mem04ErrorCode::kSessionNotFound:  return kSessionNotFound;
        case Mem04ErrorCode::kAlreadyOptedOut:  return kAlreadyOptedOut;
        case Mem04ErrorCode::kNotOptedOut:      return kNotOptedOut;
        case Mem04ErrorCode::kRevokeDenied:     return kRevokeDenied;
        case Mem04ErrorCode::kOptOutDisabled:   return kOptOutDisabled;
        case Mem04ErrorCode::kInvalidSessionId: return kInvalidSessionId;
        case Mem04ErrorCode::kMetadataTooLarge: return kMetadataTooLarge;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kSessionNotFound;
}

const char* Mem04ErrorCodeString(Mem04ErrorCode code) {
    return GetMem04ErrorInfo(code).cx_code;
}

int Mem04ErrorHttpStatus(Mem04ErrorCode code) {
    return GetMem04ErrorInfo(code).http_status;
}

const std::vector<std::string>& RequiredStructuredDataKeys(Mem04ErrorCode code) {
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
        case Mem04ErrorCode::kSessionNotFound:  return kSessionNotFoundKeys;
        case Mem04ErrorCode::kAlreadyOptedOut:  return kAlreadyOptedOutKeys;
        case Mem04ErrorCode::kNotOptedOut:      return kNotOptedOutKeys;
        case Mem04ErrorCode::kRevokeDenied:     return kRevokeDeniedKeys;
        case Mem04ErrorCode::kOptOutDisabled:   return kOptOutDisabledKeys;
        case Mem04ErrorCode::kInvalidSessionId: return kInvalidSessionIdKeys;
        case Mem04ErrorCode::kMetadataTooLarge: return kMetadataTooLargeKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(Mem04ErrorCode code,
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

AgentFriendlyError MakeMem04Error(Mem04ErrorCode code,
                                  nlohmann::json structured_data,
                                  const std::string& message) {
    const Mem04ErrorInfo& info = GetMem04ErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode Mem04ErrorToStatusCode(Mem04ErrorCode code) {
    switch (code) {
        // session row absent → kNotFound (the 404 surface).
        case Mem04ErrorCode::kSessionNotFound:  return StatusCode::kNotFound;
        // already opted-out (409) → kAlreadyExists is the closest coarse code (the
        // resource is already in the requested state); rich category=permanent + the
        // CX_ERR_ token + MakeMem04Error preserve the 409 semantics at the boundary.
        case Mem04ErrorCode::kAlreadyOptedOut:  return StatusCode::kAlreadyExists;
        // revoke on an active session (409 Conflict — there is nothing to revoke) →
        // kAlreadyExists (terminal-state conflict, same coarse bucket as above).
        case Mem04ErrorCode::kNotOptedOut:      return StatusCode::kAlreadyExists;
        // missing admin scope → kPermissionDenied (the 403 surface).
        case Mem04ErrorCode::kRevokeDenied:     return StatusCode::kPermissionDenied;
        // feature disabled by config (503) → kUnavailable (the request can succeed once
        // mem04.enabled flips); rich category=permanent preserved via the CX_ERR_ token.
        case Mem04ErrorCode::kOptOutDisabled:   return StatusCode::kUnavailable;
        // malformed session id / oversize metadata (422) → kInvalidArgument (the
        // request itself is invalid; not retryable as-is).
        case Mem04ErrorCode::kInvalidSessionId: return StatusCode::kInvalidArgument;
        case Mem04ErrorCode::kMetadataTooLarge: return StatusCode::kInvalidArgument;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status Mem04Status(Mem04ErrorCode code, const std::string& detail) {
    const char* cx = Mem04ErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(Mem04ErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::memory::immunity
