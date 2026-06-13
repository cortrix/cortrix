#include "cortrix/agent_trace/agent_trace_error.h"

#include <utility>

namespace cortrix::agent_trace {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (F13 §9.2). Defined as function-local statics so each
// returns a stable reference. The switches below are intentionally exhaustive:
// building with -Wall -Wextra (-Wswitch) turns "added a code without a row" into a
// warning, which the project treats as a build failure — the registry can't
// silently drift from the enum.
//
// retry_after_ms: every F13 code is "-" in §9.2 (no machine retry delay). Only
// INTERNAL is retryable (transient) — the Agent may retry but no fixed backoff is
// promised; the rest are permanent/auth (caller must fix the request).
constexpr F13ErrorInfo kSessionNotFound{
    "CX_ERR_F13_SESSION_NOT_FOUND", ErrorCategory::kPermanent, false, std::nullopt};
constexpr F13ErrorInfo kInvalidFilter{
    "CX_ERR_F13_INVALID_FILTER", ErrorCategory::kPermanent, false, std::nullopt};
constexpr F13ErrorInfo kInteractionNotFound{
    "CX_ERR_F13_INTERACTION_NOT_FOUND", ErrorCategory::kPermanent, false, std::nullopt};
constexpr F13ErrorInfo kUnauthorized{
    "CX_ERR_F13_UNAUTHORIZED", ErrorCategory::kAuth, false, std::nullopt};
constexpr F13ErrorInfo kMcpSessionInvalid{
    "CX_ERR_F13_MCP_SESSION_INVALID", ErrorCategory::kPermanent, false, std::nullopt};
constexpr F13ErrorInfo kSessionExpired{
    "CX_ERR_F13_SESSION_EXPIRED", ErrorCategory::kPermanent, false, std::nullopt};
constexpr F13ErrorInfo kInternal{
    "CX_ERR_F13_INTERNAL", ErrorCategory::kTransient, true, std::nullopt};

}  // namespace

const F13ErrorInfo& GetF13ErrorInfo(F13ErrorCode code) {
    switch (code) {
        case F13ErrorCode::kSessionNotFound:     return kSessionNotFound;
        case F13ErrorCode::kInvalidFilter:       return kInvalidFilter;
        case F13ErrorCode::kInteractionNotFound: return kInteractionNotFound;
        case F13ErrorCode::kUnauthorized:        return kUnauthorized;
        case F13ErrorCode::kMcpSessionInvalid:   return kMcpSessionInvalid;
        case F13ErrorCode::kSessionExpired:      return kSessionExpired;
        case F13ErrorCode::kInternal:            return kInternal;
    }
    return kInternal;  // unreachable for a valid enum value
}

const char* F13ErrorCodeString(F13ErrorCode code) {
    return GetF13ErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(F13ErrorCode code) {
    // §9.2 structured_data column, 1:1. Function-local statics → stable references.
    // INVALID_FILTER's value_preview is optional (PII guard) → not required.
    static const std::vector<std::string> kSession{"session_id"};
    static const std::vector<std::string> kFilter{"invalid_field", "reason"};
    static const std::vector<std::string> kInteraction{"interaction_id"};
    static const std::vector<std::string> kRole{"required_role"};
    static const std::vector<std::string> kMcp{"session_id", "reason"};
    static const std::vector<std::string> kExpired{"session_id", "retention_days"};
    static const std::vector<std::string> kErrId{"error_id"};

    switch (code) {
        case F13ErrorCode::kSessionNotFound:     return kSession;
        case F13ErrorCode::kInvalidFilter:       return kFilter;
        case F13ErrorCode::kInteractionNotFound: return kInteraction;
        case F13ErrorCode::kUnauthorized:        return kRole;
        case F13ErrorCode::kMcpSessionInvalid:   return kMcp;
        case F13ErrorCode::kSessionExpired:      return kExpired;
        case F13ErrorCode::kInternal:            return kErrId;
    }
    return kErrId;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(F13ErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeF13Error(F13ErrorCode code,
                                nlohmann::json structured_data,
                                const std::string& message) {
    const F13ErrorInfo& info = GetF13ErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode F13ErrorToStatusCode(F13ErrorCode code) {
    switch (code) {
        // Missing session / interaction → kNotFound.
        case F13ErrorCode::kSessionNotFound:
        case F13ErrorCode::kInteractionNotFound:
            return StatusCode::kNotFound;
        // Bad filter / invalid MCP session id / expired session → caller-side bad
        // input → kInvalidArgument. The rich CX_ERR_F13_* identity + category
        // survive via the token + boundary MakeF13Error().
        case F13ErrorCode::kInvalidFilter:
        case F13ErrorCode::kMcpSessionInvalid:
        case F13ErrorCode::kSessionExpired:
            return StatusCode::kInvalidArgument;
        // Cross-user denial → kPermissionDenied (auth category).
        case F13ErrorCode::kUnauthorized:
            return StatusCode::kPermissionDenied;
        // Unexpected server fault → kInternal (transient/retryable).
        case F13ErrorCode::kInternal:
            return StatusCode::kInternal;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status F13Status(F13ErrorCode code, const std::string& detail) {
    const char* cx = F13ErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(F13ErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::agent_trace
