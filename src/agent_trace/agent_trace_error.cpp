#include "cortrix/agent_trace/agent_trace_error.h"

#include <utility>

namespace cortrix::agent_trace {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so each
// returns a stable reference. The switches below are intentionally exhaustive:
// building with -Wall -Wextra (-Wswitch) turns "added a code without a row" into a
// warning, which the project treats as a build failure — the registry can't
// silently drift from the enum.
//
// retry_after_ms: every trace code is "-" (no machine retry delay). Only
// INTERNAL is retryable (transient) — the Agent may retry but no fixed backoff is
// promised; the rest are permanent/auth (caller must fix the request).
constexpr AgentTraceErrorInfo kSessionNotFound{
    "CX_ERR_TRACE_SESSION_NOT_FOUND", ErrorCategory::kPermanent, false, std::nullopt};
constexpr AgentTraceErrorInfo kInvalidFilter{
    "CX_ERR_TRACE_INVALID_FILTER", ErrorCategory::kPermanent, false, std::nullopt};
constexpr AgentTraceErrorInfo kInteractionNotFound{
    "CX_ERR_TRACE_INTERACTION_NOT_FOUND", ErrorCategory::kPermanent, false, std::nullopt};
constexpr AgentTraceErrorInfo kUnauthorized{
    "CX_ERR_TRACE_UNAUTHORIZED", ErrorCategory::kAuth, false, std::nullopt};
constexpr AgentTraceErrorInfo kMcpSessionInvalid{
    "CX_ERR_TRACE_MCP_SESSION_INVALID", ErrorCategory::kPermanent, false, std::nullopt};
constexpr AgentTraceErrorInfo kSessionExpired{
    "CX_ERR_TRACE_SESSION_EXPIRED", ErrorCategory::kPermanent, false, std::nullopt};
constexpr AgentTraceErrorInfo kInternal{
    "CX_ERR_TRACE_INTERNAL", ErrorCategory::kTransient, true, std::nullopt};

}  // namespace

const AgentTraceErrorInfo& GetAgentTraceErrorInfo(AgentTraceErrorCode code) {
    switch (code) {
        case AgentTraceErrorCode::kSessionNotFound:     return kSessionNotFound;
        case AgentTraceErrorCode::kInvalidFilter:       return kInvalidFilter;
        case AgentTraceErrorCode::kInteractionNotFound: return kInteractionNotFound;
        case AgentTraceErrorCode::kUnauthorized:        return kUnauthorized;
        case AgentTraceErrorCode::kMcpSessionInvalid:   return kMcpSessionInvalid;
        case AgentTraceErrorCode::kSessionExpired:      return kSessionExpired;
        case AgentTraceErrorCode::kInternal:            return kInternal;
    }
    return kInternal;  // unreachable for a valid enum value
}

const char* AgentTraceErrorCodeString(AgentTraceErrorCode code) {
    return GetAgentTraceErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(AgentTraceErrorCode code) {
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
        case AgentTraceErrorCode::kSessionNotFound:     return kSession;
        case AgentTraceErrorCode::kInvalidFilter:       return kFilter;
        case AgentTraceErrorCode::kInteractionNotFound: return kInteraction;
        case AgentTraceErrorCode::kUnauthorized:        return kRole;
        case AgentTraceErrorCode::kMcpSessionInvalid:   return kMcp;
        case AgentTraceErrorCode::kSessionExpired:      return kExpired;
        case AgentTraceErrorCode::kInternal:            return kErrId;
    }
    return kErrId;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(AgentTraceErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeAgentTraceError(AgentTraceErrorCode code,
                                nlohmann::json structured_data,
                                const std::string& message) {
    const AgentTraceErrorInfo& info = GetAgentTraceErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode AgentTraceErrorToStatusCode(AgentTraceErrorCode code) {
    switch (code) {
        // Missing session / interaction → kNotFound.
        case AgentTraceErrorCode::kSessionNotFound:
        case AgentTraceErrorCode::kInteractionNotFound:
            return StatusCode::kNotFound;
        // Bad filter / invalid MCP session id / expired session → caller-side bad
        // input → kInvalidArgument. The rich CX_ERR_TRACE_* identity + category
        // survive via the token + boundary MakeAgentTraceError().
        case AgentTraceErrorCode::kInvalidFilter:
        case AgentTraceErrorCode::kMcpSessionInvalid:
        case AgentTraceErrorCode::kSessionExpired:
            return StatusCode::kInvalidArgument;
        // Cross-user denial → kPermissionDenied (auth category).
        case AgentTraceErrorCode::kUnauthorized:
            return StatusCode::kPermissionDenied;
        // Unexpected server fault → kInternal (transient/retryable).
        case AgentTraceErrorCode::kInternal:
            return StatusCode::kInternal;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status AgentTraceStatus(AgentTraceErrorCode code, const std::string& detail) {
    const char* cx = AgentTraceErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(AgentTraceErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::agent_trace
