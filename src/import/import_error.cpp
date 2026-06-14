#include "cortrix/import/import_error.h"

#include <utility>

namespace cortrix::import {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (F16a §5.4). Defined as function-local statics so each
// returns a stable reference. The switch in GetF16aErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without a
// row" into a warning (treated as a build failure), so the registry can't silently
// drift from the enum.
//
// retry_after_ms: §5.4 marks CONNECTION_FAILED (transient) + TIMEOUT (timeout) as
// retryable. CONNECTION_FAILED gets a short 5s backoff (network blip); TIMEOUT gets
// 30s (the §5.3 example body) — a re-run of a slow query benefits from a longer
// pause. The 4 non-retryable codes carry null.
constexpr F16aErrorInfo kConnectionFailed
    {"CX_ERR_F16A_CONNECTION_FAILED",   ErrorCategory::kTransient, true,  5000,         503};
constexpr F16aErrorInfo kAuthDenied
    {"CX_ERR_F16A_AUTH_DENIED",         ErrorCategory::kAuth,      false, std::nullopt, 403};
constexpr F16aErrorInfo kInvalidSql
    {"CX_ERR_F16A_INVALID_SQL",         ErrorCategory::kPermanent, false, std::nullopt, 400};
constexpr F16aErrorInfo kTimeout
    {"CX_ERR_F16A_TIMEOUT",             ErrorCategory::kTimeout,   true,  30000,        504};
constexpr F16aErrorInfo kRowsLimitExceeded
    {"CX_ERR_F16A_ROWS_LIMIT_EXCEEDED", ErrorCategory::kQuota,     false, std::nullopt, 413};
constexpr F16aErrorInfo kCrossTenantRef
    {"CX_ERR_F16A_CROSS_TENANT_REF",    ErrorCategory::kAuth,      false, std::nullopt, 403};
// [R2-M5] Catch-all for an unexpected import throw (DB driver / allocation). Transient +
// retryable with a short backoff — a transient cause may clear on a re-run, and the
// retryable<->retry_after_ms invariant (GEN-Agent #6, test_import_error) requires a value.
constexpr F16aErrorInfo kInternal
    {"CX_ERR_F16A_INTERNAL",            ErrorCategory::kTransient, true,  1000,         500};

}  // namespace

const F16aErrorInfo& GetF16aErrorInfo(F16aErrorCode code) {
    switch (code) {
        case F16aErrorCode::kConnectionFailed:  return kConnectionFailed;
        case F16aErrorCode::kAuthDenied:        return kAuthDenied;
        case F16aErrorCode::kInvalidSql:        return kInvalidSql;
        case F16aErrorCode::kTimeout:           return kTimeout;
        case F16aErrorCode::kRowsLimitExceeded: return kRowsLimitExceeded;
        case F16aErrorCode::kCrossTenantRef:    return kCrossTenantRef;
        case F16aErrorCode::kInternal:          return kInternal;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInvalidSql;
}

const char* F16aErrorCodeString(F16aErrorCode code) {
    return GetF16aErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(F16aErrorCode code) {
    // §5.3 / §5.4 structured_data keys the Agent needs to act. Function-local
    // statics → stable refs.
    static const std::vector<std::string> kEmpty{};
    // §5.3 example body for TIMEOUT carries task_id + timeout_seconds +
    // rows_imported_before_timeout so the Agent can resume / narrow the query.
    static const std::vector<std::string> kTimeoutKeys{
        "task_id", "timeout_seconds", "rows_imported_before_timeout"};
    // ROWS_LIMIT: the Agent slices the query under the cap (R5 — needs the cap +
    // the estimate that tripped it).
    static const std::vector<std::string> kRowsKeys{"max_rows", "estimated_rows"};
    // CROSS_TENANT_REF: which ref / tenant pair was rejected (D7 — Agent re-picks a
    // ref owned by the caller's tenant).
    static const std::vector<std::string> kCrossTenantKeys{"connection_ref", "tenant_id"};

    switch (code) {
        // 503 transient: body is contextual (host/db live in the message).
        case F16aErrorCode::kConnectionFailed:  return kEmpty;
        // 403 auth: contextual (namespace named in the message).
        case F16aErrorCode::kAuthDenied:        return kEmpty;
        // 400 permanent: contextual (the offending keyword / DSL operator is in the
        // message; we deliberately do NOT echo the raw SQL back — R2 credential /
        // payload hygiene).
        case F16aErrorCode::kInvalidSql:        return kEmpty;
        case F16aErrorCode::kTimeout:           return kTimeoutKeys;
        case F16aErrorCode::kRowsLimitExceeded: return kRowsKeys;
        case F16aErrorCode::kCrossTenantRef:    return kCrossTenantKeys;
        case F16aErrorCode::kInternal:          return kEmpty;  // contextual (detail in message)
    }
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(F16aErrorCode code,
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

AgentFriendlyError MakeF16aError(F16aErrorCode code,
                                 nlohmann::json structured_data,
                                 const std::string& message) {
    const F16aErrorInfo& info = GetF16aErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode F16aErrorToStatusCode(F16aErrorCode code) {
    switch (code) {
        // network blip → kUnavailable.
        case F16aErrorCode::kConnectionFailed:  return StatusCode::kUnavailable;
        // auth / cross-tenant → kPermissionDenied.
        case F16aErrorCode::kAuthDenied:
        case F16aErrorCode::kCrossTenantRef:    return StatusCode::kPermissionDenied;
        // bad SQL / DSL → kInvalidArgument.
        case F16aErrorCode::kInvalidSql:        return StatusCode::kInvalidArgument;
        // timeout → kUnavailable (closest coarse code; rich category=timeout
        // preserved via the CX_ERR_ token + boundary MakeF16aError).
        case F16aErrorCode::kTimeout:           return StatusCode::kUnavailable;
        // quota → kInvalidArgument (closest coarse code; rich category=quota
        // preserved via the token).
        case F16aErrorCode::kRowsLimitExceeded: return StatusCode::kInvalidArgument;
        case F16aErrorCode::kInternal:          return StatusCode::kInternal;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status F16aStatus(F16aErrorCode code, const std::string& detail) {
    const char* cx = F16aErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(F16aErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::import
