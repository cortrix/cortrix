#include "cortrix/auth/auth_error.h"

#include <utility>

namespace cortrix::auth {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (6-field table). Defined as constexpr
// statics so each returns a stable reference. The switch in GetAuthErrorInfo is
// intentionally exhaustive: building with -Wall -Wextra (-Wswitch) turns "added
// a code without a row" into a warning, which the project treats as a build
// failure — the registry can't silently drift from the enum.
//
// retry_after_ms below is the *static* default; kAccountLocked / kRateLimited
// carry a live value (remaining lockout / reset ms) supplied per-call via MakeAuthError's
// retry_after_override (the table value is the fallback when none is given).
constexpr AuthErrorInfo kEmailAlreadyExists      {"CX_ERR_AUTH_EMAIL_ALREADY_EXISTS",      ErrorCategory::kPermanent, false, std::nullopt};
constexpr AuthErrorInfo kInvalidCredentials      {"CX_ERR_AUTH_INVALID_CREDENTIALS",       ErrorCategory::kAuth,      false, std::nullopt};
constexpr AuthErrorInfo kAccountLocked           {"CX_ERR_ACCOUNT_LOCKED",                 ErrorCategory::kQuota,     true,  std::nullopt};
constexpr AuthErrorInfo kAccountDisabled         {"CX_ERR_AUTH_ACCOUNT_DISABLED",          ErrorCategory::kAuth,      false, std::nullopt};
constexpr AuthErrorInfo kTokenExpired            {"CX_ERR_AUTH_TOKEN_EXPIRED",             ErrorCategory::kAuth,      false, std::nullopt};
constexpr AuthErrorInfo kTokenRevoked            {"CX_ERR_AUTH_TOKEN_REVOKED",             ErrorCategory::kAuth,      false, std::nullopt};
constexpr AuthErrorInfo kInvalidRefreshToken     {"CX_ERR_AUTH_INVALID_REFRESH_TOKEN",     ErrorCategory::kAuth,      false, std::nullopt};
constexpr AuthErrorInfo kTokenVerificationFailed {"CX_ERR_AUTH_TOKEN_VERIFICATION_FAILED", ErrorCategory::kTransient, true,  5000};
constexpr AuthErrorInfo kInvalidResetCode        {"CX_ERR_AUTH_INVALID_RESET_CODE",        ErrorCategory::kPermanent, false, std::nullopt};
constexpr AuthErrorInfo kBootstrapTokenInvalid   {"CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID",   ErrorCategory::kAuth,      false, std::nullopt};
constexpr AuthErrorInfo kInvalidApiKey           {"CX_ERR_AUTH_INVALID_API_KEY",           ErrorCategory::kAuth,      false, std::nullopt};
constexpr AuthErrorInfo kAdminRequired           {"CX_ERR_AUTH_ADMIN_REQUIRED",            ErrorCategory::kAuth,      false, std::nullopt};
constexpr AuthErrorInfo kEmailSendFailed         {"CX_ERR_AUTH_EMAIL_SEND_FAILED",         ErrorCategory::kTransient, true,  30000};
constexpr AuthErrorInfo kBcryptTimeout           {"CX_ERR_AUTH_BCRYPT_TIMEOUT",            ErrorCategory::kPermanent, false, std::nullopt};
constexpr AuthErrorInfo kJwtInitFailed           {"CX_ERR_AUTH_JWT_INIT_FAILED",           ErrorCategory::kPermanent, false, std::nullopt};
constexpr AuthErrorInfo kUserNotFound            {"CX_ERR_USER_NOT_FOUND",                 ErrorCategory::kPermanent, false, std::nullopt};
constexpr AuthErrorInfo kUserEmailExists         {"CX_ERR_USER_EMAIL_EXISTS",              ErrorCategory::kPermanent, false, std::nullopt};
constexpr AuthErrorInfo kUserValidation          {"CX_ERR_USER_VALIDATION",                ErrorCategory::kPermanent, false, std::nullopt};
constexpr AuthErrorInfo kInvalidRequest          {"CX_ERR_INVALID_REQUEST",                ErrorCategory::kPermanent, false, std::nullopt};
constexpr AuthErrorInfo kUnauthorized            {"CX_ERR_UNAUTHORIZED",                   ErrorCategory::kAuth,      false, std::nullopt};
constexpr AuthErrorInfo kNotFound                {"CX_ERR_NOT_FOUND",                      ErrorCategory::kPermanent, false, std::nullopt};
constexpr AuthErrorInfo kRateLimited             {"CX_ERR_RATE_LIMITED",                   ErrorCategory::kQuota,     true,  std::nullopt};
constexpr AuthErrorInfo kInternalError           {"CX_ERR_INTERNAL_ERROR",                 ErrorCategory::kTransient, true,  5000};
constexpr AuthErrorInfo kServiceUnavailable      {"CX_ERR_SERVICE_UNAVAILABLE",            ErrorCategory::kTransient, true,  10000};

}  // namespace

const AuthErrorInfo& GetAuthErrorInfo(AuthErrorCode code) {
    switch (code) {
        case AuthErrorCode::kEmailAlreadyExists:      return kEmailAlreadyExists;
        case AuthErrorCode::kInvalidCredentials:      return kInvalidCredentials;
        case AuthErrorCode::kAccountLocked:           return kAccountLocked;
        case AuthErrorCode::kAccountDisabled:         return kAccountDisabled;
        case AuthErrorCode::kTokenExpired:            return kTokenExpired;
        case AuthErrorCode::kTokenRevoked:            return kTokenRevoked;
        case AuthErrorCode::kInvalidRefreshToken:     return kInvalidRefreshToken;
        case AuthErrorCode::kTokenVerificationFailed: return kTokenVerificationFailed;
        case AuthErrorCode::kInvalidResetCode:        return kInvalidResetCode;
        case AuthErrorCode::kBootstrapTokenInvalid:   return kBootstrapTokenInvalid;
        case AuthErrorCode::kInvalidApiKey:           return kInvalidApiKey;
        case AuthErrorCode::kAdminRequired:           return kAdminRequired;
        case AuthErrorCode::kEmailSendFailed:         return kEmailSendFailed;
        case AuthErrorCode::kBcryptTimeout:           return kBcryptTimeout;
        case AuthErrorCode::kJwtInitFailed:           return kJwtInitFailed;
        case AuthErrorCode::kUserNotFound:            return kUserNotFound;
        case AuthErrorCode::kUserEmailExists:         return kUserEmailExists;
        case AuthErrorCode::kUserValidation:          return kUserValidation;
        case AuthErrorCode::kInvalidRequest:          return kInvalidRequest;
        case AuthErrorCode::kUnauthorized:            return kUnauthorized;
        case AuthErrorCode::kNotFound:                return kNotFound;
        case AuthErrorCode::kRateLimited:             return kRateLimited;
        case AuthErrorCode::kInternalError:           return kInternalError;
        case AuthErrorCode::kServiceUnavailable:      return kServiceUnavailable;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInternalError;
}

const char* AuthErrorCodeString(AuthErrorCode code) {
    return GetAuthErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(AuthErrorCode code) {
    // The "structured_data" column, 1:1. Function-local statics → stable
    // references. Empty vector where the spec shows `{}` (intentionally empty,
    // e.g. kInvalidCredentials prevents account enumeration).
    static const std::vector<std::string> kEmpty{};
    static const std::vector<std::string> kHint{"hint"};
    static const std::vector<std::string> kReason{"reason"};
    static const std::vector<std::string> kTokenExpired{"expired_at", "refresh_available"};
    static const std::vector<std::string> kLocked{"locked_until", "attempts_remaining"};
    static const std::vector<std::string> kRotation{"rotation_in_progress"};
    static const std::vector<std::string> kAdmin{"required_role", "current_role"};
    static const std::vector<std::string> kUserId{"user_id"};
    static const std::vector<std::string> kEmail{"email"};
    static const std::vector<std::string> kFieldReason{"field", "reason"};
    static const std::vector<std::string> kRulesViolated{"rules_violated"};
    static const std::vector<std::string> kRateLimited{"limit", "remaining", "reset_at"};
    static const std::vector<std::string> kReqComp{"request_id", "component"};
    static const std::vector<std::string> kSmtp{"smtp_error", "request_id"};
    static const std::vector<std::string> kBcrypt{"request_id", "cost"};
    static const std::vector<std::string> kJwtInit{"component"};
    static const std::vector<std::string> kComponent{"component"};

    switch (code) {
        case AuthErrorCode::kEmailAlreadyExists:      return kHint;          // { hint: "use_login_or_reset" }
        case AuthErrorCode::kInvalidCredentials:      return kEmpty;         // {} anti-enumeration
        case AuthErrorCode::kAccountLocked:           return kLocked;
        case AuthErrorCode::kAccountDisabled:         return kReason;
        case AuthErrorCode::kTokenExpired:            return kTokenExpired;
        case AuthErrorCode::kTokenRevoked:            return kReason;
        case AuthErrorCode::kInvalidRefreshToken:     return kReason;
        case AuthErrorCode::kTokenVerificationFailed: return kRotation;
        case AuthErrorCode::kInvalidResetCode:        return kReason;        // { reason: expired|invalid|reused }
        case AuthErrorCode::kBootstrapTokenInvalid:   return kReason;        // { reason: expired|used|not_found }
        case AuthErrorCode::kInvalidApiKey:           return kReason;        // { reason: revoked|expired|not_found }
        case AuthErrorCode::kAdminRequired:           return kAdmin;
        case AuthErrorCode::kEmailSendFailed:         return kSmtp;
        case AuthErrorCode::kBcryptTimeout:           return kBcrypt;
        case AuthErrorCode::kJwtInitFailed:           return kJwtInit;       // { component: "platform_db" }
        case AuthErrorCode::kUserNotFound:            return kUserId;
        case AuthErrorCode::kUserEmailExists:         return kEmail;
        case AuthErrorCode::kUserValidation:          return kFieldReason;
        case AuthErrorCode::kInvalidRequest:          return kRulesViolated;
        case AuthErrorCode::kUnauthorized:            return kReason;
        case AuthErrorCode::kNotFound:                return kEmpty;
        case AuthErrorCode::kRateLimited:             return kRateLimited;
        case AuthErrorCode::kInternalError:           return kReqComp;
        case AuthErrorCode::kServiceUnavailable:      return kComponent;
    }
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(AuthErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeAuthError(AuthErrorCode code,
                                 nlohmann::json structured_data,
                                 const std::string& message,
                                 std::optional<int> retry_after_override) {
    const AuthErrorInfo& info = GetAuthErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    // Live retry_after (remaining lockout / reset ms) wins over the table default; this is
    // how CX_ERR_ACCOUNT_LOCKED / CX_ERR_RATE_LIMITED carry a per-request value
    //. Otherwise fall back to the registry's static value.
    err.retry_after_ms = retry_after_override.has_value() ? retry_after_override
                                                          : info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode AuthErrorToStatusCode(AuthErrorCode code) {
    switch (code) {
        // "not found" → kNotFound.
        case AuthErrorCode::kNotFound:
        case AuthErrorCode::kUserNotFound:
            return StatusCode::kNotFound;
        // "already exists" → kAlreadyExists.
        case AuthErrorCode::kEmailAlreadyExists:
        case AuthErrorCode::kUserEmailExists:
            return StatusCode::kAlreadyExists;
        // auth-category (credentials / tokens / api keys / admin) → kUnauthenticated.
        case AuthErrorCode::kInvalidCredentials:
        case AuthErrorCode::kTokenExpired:
        case AuthErrorCode::kTokenRevoked:
        case AuthErrorCode::kInvalidRefreshToken:
        case AuthErrorCode::kTokenVerificationFailed:
        case AuthErrorCode::kBootstrapTokenInvalid:
        case AuthErrorCode::kInvalidApiKey:
        case AuthErrorCode::kUnauthorized:
            return StatusCode::kUnauthenticated;
        // locked / disabled / admin-required → kPermissionDenied (the coarse code;
        // rich category=quota/auth is preserved via the CX_ERR_ token + boundary
        // MakeAuthError).
        case AuthErrorCode::kAccountLocked:
        case AuthErrorCode::kAccountDisabled:
        case AuthErrorCode::kAdminRequired:
        case AuthErrorCode::kRateLimited:
            return StatusCode::kPermissionDenied;
        // caller-side / permanent validation → kInvalidArgument.
        case AuthErrorCode::kInvalidResetCode:
        case AuthErrorCode::kInvalidRequest:
        case AuthErrorCode::kUserValidation:
        case AuthErrorCode::kBcryptTimeout:
            return StatusCode::kInvalidArgument;
        // transient / retryable → kUnavailable.
        case AuthErrorCode::kEmailSendFailed:
        case AuthErrorCode::kServiceUnavailable:
            return StatusCode::kUnavailable;
        // startup / generic internal → kInternal.
        case AuthErrorCode::kJwtInitFailed:
        case AuthErrorCode::kInternalError:
            return StatusCode::kInternal;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status AuthStatus(AuthErrorCode code, const std::string& detail) {
    const char* cx = AuthErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(AuthErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::auth
