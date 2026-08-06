#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::auth {

/// The Auth error identities. Each maps to a stable
/// `CX_ERR_*` string + a GEN-Agent category + retryability via the canonical
/// registry below. This is the exact convention-compliant mirror of
/// catalog::CatalogErrorCode (CODING_CONVENTIONS §3 / L1-α §2): an *enum of
/// identities* turned into the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError by MakeAuthError() — we do NOT
/// introduce a parallel AuthError struct.
///
/// Module-prefix rule: auth-domain codes
/// carry the `CX_ERR_AUTH_` second-level prefix. A handful of codes this layer *uses*
/// are project-shared and keep their generic `CX_ERR_` spelling (governed by the
/// master table, V14 J6): kInvalidRequest / kUnauthorized / kNotFound / kRateLimited /
/// kInternalError / kServiceUnavailable, plus the admin/users codes
/// kUserNotFound / kUserEmailExists / kUserValidation. They live
/// in this enum so every error the auth layer returns flows through one registry, exactly
/// as CatalogErrorCode carries kInternalError / kNotImplemented.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class AuthErrorCode {
    // --- Registration / credentials ---
    kEmailAlreadyExists,          // 409  CX_ERR_AUTH_EMAIL_ALREADY_EXISTS
    kInvalidCredentials,          // 401  CX_ERR_AUTH_INVALID_CREDENTIALS
    kAccountLocked,               // 403  CX_ERR_ACCOUNT_LOCKED   (shared, retry_after_ms)
    kAccountDisabled,             // 403  CX_ERR_AUTH_ACCOUNT_DISABLED
    // --- Tokens (JWT access / refresh) ---
    kTokenExpired,                // 401  CX_ERR_AUTH_TOKEN_EXPIRED
    kTokenRevoked,                // 401  CX_ERR_AUTH_TOKEN_REVOKED
    kInvalidRefreshToken,         // 401  CX_ERR_AUTH_INVALID_REFRESH_TOKEN
    kTokenVerificationFailed,     // 401  CX_ERR_AUTH_TOKEN_VERIFICATION_FAILED (rotation window)
    // --- Reset / verification codes ---
    kInvalidResetCode,            // 400  CX_ERR_AUTH_INVALID_RESET_CODE
    // --- Bootstrap + API Key (topic 7 D) ---
    kBootstrapTokenInvalid,       // 401  CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID
    kInvalidApiKey,               // 401  CX_ERR_AUTH_INVALID_API_KEY
    kAdminRequired,               // 403  CX_ERR_AUTH_ADMIN_REQUIRED
    // --- Email / bcrypt / startup (topic 5 / topic 1.1 C) ---
    kEmailSendFailed,             // 500  CX_ERR_AUTH_EMAIL_SEND_FAILED
    kBcryptTimeout,               // 500  CX_ERR_AUTH_BCRYPT_TIMEOUT
    kJwtInitFailed,               // 500  CX_ERR_AUTH_JWT_INIT_FAILED
    // --- admin/users 5 endpoints (§2.13-bis, V14 J6 genuinely new codes) ---
    kUserNotFound,                // 404  CX_ERR_USER_NOT_FOUND
    kUserEmailExists,             // 409  CX_ERR_USER_EMAIL_EXISTS
    kUserValidation,              // 422  CX_ERR_USER_VALIDATION
    // --- Project-shared codes this layer uses (governed by the master table, generic prefix) ---
    kInvalidRequest,              // 400  CX_ERR_INVALID_REQUEST
    kUnauthorized,                // 401  CX_ERR_UNAUTHORIZED
    kNotFound,                    // 404  CX_ERR_NOT_FOUND
    kRateLimited,                 // 429  CX_ERR_RATE_LIMITED
    kInternalError,               // 500  CX_ERR_INTERNAL_ERROR
    kServiceUnavailable,          // 503  CX_ERR_SERVICE_UNAVAILABLE
};

/// Total number of auth error codes (23 scenarios mapped to
/// 24 unique identities incl. shared ones). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kAuthErrorCodeCount = 24;

/// Canonical, immutable attributes of one error code.
struct AuthErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    agent_friendly::ErrorCategory category;   ///< auth/quota/transient/permanent/timeout
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per §5.2
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the rows.
const AuthErrorInfo& GetAuthErrorInfo(AuthErrorCode code);

/// The "CX_ERR_*" string for `code` (convenience over GetAuthErrorInfo).
const char* AuthErrorCodeString(AuthErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry
/// (structured_data column). Empty where the spec lists `{}` (e.g.
/// kInvalidCredentials — empty to prevent enumeration). SoT for the
/// Agent-friendly contract (GEN-Agent #5).
const std::vector<std::string>& RequiredStructuredDataKeys(AuthErrorCode code);

/// True iff `structured_data` contains every required key for `code`. Used to
/// assert Agent-friendly completeness before returning an error body.
bool HasRequiredStructuredData(AuthErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable /
/// retry_after_ms are filled from the canonical registry — call sites never
/// restate them. For kAccountLocked / kRateLimited the caller passes the live
/// retry_after_ms via `retry_after_override` (remaining lockout ms).
agent_friendly::AgentFriendlyError MakeAuthError(
    AuthErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "",
    std::optional<int> retry_after_override = std::nullopt);

/// Bridge an auth error to a plain Status for the Result<T>/Status interface
/// surface (F-FREEZE-1). The StatusCode is the coarse mapping of `code`; the
/// message is prefixed with the CX_ERR_* token ("CX_ERR_X: detail") so the exact
/// auth identity is recoverable at the API/SDK boundary, which re-inflates the
/// full Agent-friendly body via MakeAuthError(). Mirrors catalog::CatalogStatus.
Status AuthStatus(AuthErrorCode code, const std::string& detail = "");

/// Coarse AuthErrorCode → StatusCode mapping used by AuthStatus (exposed for
/// tests / boundary code that needs the mapping without a message).
StatusCode AuthErrorToStatusCode(AuthErrorCode code);

}  // namespace cortrix::auth
