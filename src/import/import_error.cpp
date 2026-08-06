#include "cortrix/import/import_error.h"

#include <utility>

namespace cortrix::import {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so each
// returns a stable reference. The switch in GetImportErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without a
// row" into a warning (treated as a build failure), so the registry can't silently
// drift from the enum.
//
// retry_after_ms: marks CONNECTION_FAILED (transient) + TIMEOUT (timeout) as
// retryable. CONNECTION_FAILED gets a short 5s backoff (network blip); TIMEOUT gets
// 30s (the example body) — a re-run of a slow query benefits from a longer
// pause. The 4 non-retryable codes carry null.
constexpr ImportErrorInfo kConnectionFailed
    {"CX_ERR_IMPORT_CONNECTION_FAILED",   ErrorCategory::kTransient, true,  5000,         503};
constexpr ImportErrorInfo kAuthDenied
    {"CX_ERR_IMPORT_AUTH_DENIED",         ErrorCategory::kAuth,      false, std::nullopt, 403};
constexpr ImportErrorInfo kInvalidSql
    {"CX_ERR_IMPORT_INVALID_SQL",         ErrorCategory::kPermanent, false, std::nullopt, 400};
constexpr ImportErrorInfo kTimeout
    {"CX_ERR_IMPORT_TIMEOUT",             ErrorCategory::kTimeout,   true,  30000,        504};
constexpr ImportErrorInfo kRowsLimitExceeded
    {"CX_ERR_IMPORT_ROWS_LIMIT_EXCEEDED", ErrorCategory::kQuota,     false, std::nullopt, 413};
constexpr ImportErrorInfo kCrossTenantRef
    {"CX_ERR_IMPORT_CROSS_TENANT_REF",    ErrorCategory::kAuth,      false, std::nullopt, 403};
// [R2-M5] Catch-all for an unexpected import throw (DB driver / allocation). Transient +
// retryable with a short backoff — a transient cause may clear on a re-run, and the
// retryable<->retry_after_ms invariant (GEN-Agent #6, test_import_error) requires a value.
constexpr ImportErrorInfo kInternal
    {"CX_ERR_IMPORT_INTERNAL",            ErrorCategory::kTransient, true,  1000,         500};

}  // namespace

const ImportErrorInfo& GetImportErrorInfo(ImportErrorCode code) {
    switch (code) {
        case ImportErrorCode::kConnectionFailed:  return kConnectionFailed;
        case ImportErrorCode::kAuthDenied:        return kAuthDenied;
        case ImportErrorCode::kInvalidSql:        return kInvalidSql;
        case ImportErrorCode::kTimeout:           return kTimeout;
        case ImportErrorCode::kRowsLimitExceeded: return kRowsLimitExceeded;
        case ImportErrorCode::kCrossTenantRef:    return kCrossTenantRef;
        case ImportErrorCode::kInternal:          return kInternal;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInvalidSql;
}

const char* ImportErrorCodeString(ImportErrorCode code) {
    return GetImportErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(ImportErrorCode code) {
    // structured_data keys the Agent needs to act. Function-local
    // statics → stable refs.
    static const std::vector<std::string> kEmpty{};
    // example body for TIMEOUT carries task_id + timeout_seconds +
    // rows_imported_before_timeout so the Agent can resume / narrow the query.
    static const std::vector<std::string> kTimeoutKeys{
        "task_id", "timeout_seconds", "rows_imported_before_timeout"};
    // ROWS_LIMIT: the Agent slices the query under the cap (R5 — needs the cap +
    // the estimate that tripped it).
    static const std::vector<std::string> kRowsKeys{"max_rows", "estimated_rows"};
    // CROSS_TENANT_REF: which ref / tenant pair was rejected (Agent re-picks a
    // ref owned by the caller's tenant).
    static const std::vector<std::string> kCrossTenantKeys{"connection_ref", "tenant_id"};

    switch (code) {
        // 503 transient: body is contextual (host/db live in the message).
        case ImportErrorCode::kConnectionFailed:  return kEmpty;
        // 403 auth: contextual (namespace named in the message).
        case ImportErrorCode::kAuthDenied:        return kEmpty;
        // 400 permanent: contextual (the offending keyword / DSL operator is in the
        // message; we deliberately do NOT echo the raw SQL back — R2 credential /
        // payload hygiene).
        case ImportErrorCode::kInvalidSql:        return kEmpty;
        case ImportErrorCode::kTimeout:           return kTimeoutKeys;
        case ImportErrorCode::kRowsLimitExceeded: return kRowsKeys;
        case ImportErrorCode::kCrossTenantRef:    return kCrossTenantKeys;
        case ImportErrorCode::kInternal:          return kEmpty;  // contextual (detail in message)
    }
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(ImportErrorCode code,
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

AgentFriendlyError MakeImportError(ImportErrorCode code,
                                 nlohmann::json structured_data,
                                 const std::string& message) {
    const ImportErrorInfo& info = GetImportErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode ImportErrorToStatusCode(ImportErrorCode code) {
    switch (code) {
        // network blip → kUnavailable.
        case ImportErrorCode::kConnectionFailed:  return StatusCode::kUnavailable;
        // auth / cross-tenant → kPermissionDenied.
        case ImportErrorCode::kAuthDenied:
        case ImportErrorCode::kCrossTenantRef:    return StatusCode::kPermissionDenied;
        // bad SQL / DSL → kInvalidArgument.
        case ImportErrorCode::kInvalidSql:        return StatusCode::kInvalidArgument;
        // timeout → kUnavailable (closest coarse code; rich category=timeout
        // preserved via the CX_ERR_ token + boundary MakeImportError).
        case ImportErrorCode::kTimeout:           return StatusCode::kUnavailable;
        // quota → kInvalidArgument (closest coarse code; rich category=quota
        // preserved via the token).
        case ImportErrorCode::kRowsLimitExceeded: return StatusCode::kInvalidArgument;
        case ImportErrorCode::kInternal:          return StatusCode::kInternal;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status ImportStatus(ImportErrorCode code, const std::string& detail) {
    const char* cx = ImportErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(ImportErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::import
