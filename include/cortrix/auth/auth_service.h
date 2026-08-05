#pragma once
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "cortrix/auth/auth_token.h"
#include "cortrix/auth/email_sender.h"
#include "cortrix/auth/password_utils.h"
#include "cortrix/auth/user_info.h"
#include "cortrix/config/auth_config.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::auth {

/// Core authentication service over platform.db. Owns the
/// registration / login / token / reset flows. Built up Story-by-Story:
///   - S2: Register + GetUserInfo (+ the IPasswordHasher seam).
///   - S3 (this): Login / Logout / RefreshAccessToken / ValidateAccessToken (JWT
///     HS256 via OpenSSL — team-lead approved 2026-05-31) + account lockout +
///     token blacklist.
///   - S5 adds RequestPasswordReset / ConfirmPasswordReset / VerifyEmail.
///
/// The service borrows an open platform.db handle + an IPasswordHasher (bcrypt
/// impl injected so the flow is independent of the library choice). The JWT
/// signing secret is supplied at construction (from S1's JwtSecretService). S7
/// will swap the single secret for the dual-key window via SetAcceptSecrets().
/// It does NOT own the db or hasher.
///
/// Concurrency: the in-memory token blacklist is mutex-guarded; SQLite handles
/// row locking. Concurrent registration of the same email is resolved by the
/// `users.email` UNIQUE constraint.
class AuthService {
public:
    /// `email_sender` may be null → an internal NullEmailSender is used (
    /// default; codes are logged, not delivered). S5 password-reset / email-verify
    /// route verification codes through it.
    AuthService(sqlite3* platform_db, config::AuthConfig config,
                IPasswordHasher* hasher, std::string jwt_secret,
                IEmailSender* email_sender = nullptr)
        : db_(platform_db), config_(std::move(config)), hasher_(hasher),
          sign_secret_(std::move(jwt_secret)), accept_secrets_{sign_secret_},
          email_sender_(email_sender) {}

    // ----------------------------- S2 -----------------------------
    Result<UserInfo> Register(const std::string& email,
                              const std::string& password,
                              const std::string& display_name);
    Result<UserInfo> GetUserInfo(const std::string& user_id);

    // ----------------------------- S3 -----------------------------

    /// Login: look up user → check status/lockout → verify
    /// password (IPasswordHasher) → on failure bump login_attempts (lock at
    /// max_login_attempts for lockout_duration) → on success reset attempts,
    /// issue access+refresh JWT, persist refresh_token hash. Errors:
    /// CX_ERR_AUTH_INVALID_CREDENTIALS (wrong email/pwd, anti-enumeration) /
    /// CX_ERR_ACCOUNT_LOCKED (with live retry_after_ms) / CX_ERR_AUTH_ACCOUNT_DISABLED.
    Result<AuthTokenPair> Login(const std::string& email, const std::string& password);

    /// Logout: blacklist the access token (memory + persist to
    /// token_blacklist) and revoke the associated refresh_token(s) for the sid.
    /// A malformed/already-invalid token is a no-op success (idempotent logout).
    Status Logout(const std::string& access_token);

    /// Refresh: decode refresh token → verify it is a refresh
    /// token, present + not revoked + not expired in refresh_tokens, hash matches,
    /// user still active → issue a NEW access token using the user's CURRENT
    /// tenant_id/role from the db. Errors: CX_ERR_AUTH_INVALID_REFRESH_TOKEN /
    /// CX_ERR_AUTH_TOKEN_REVOKED / CX_ERR_AUTH_TOKEN_EXPIRED / CX_ERR_AUTH_ACCOUNT_DISABLED.
    Result<std::string> RefreshAccessToken(const std::string& refresh_token);

    /// Validate an access token for the middleware: decode + verify
    /// signature/expiry, reject non-access tokens, reject blacklisted tokens,
    /// then return the AuthContext. Errors: CX_ERR_UNAUTHORIZED (bad/sig/non-access)
    /// / CX_ERR_AUTH_TOKEN_EXPIRED / CX_ERR_AUTH_TOKEN_REVOKED.
    Result<AuthContext> ValidateAccessToken(const std::string& access_token);

    /// Load the persisted (non-expired) token_blacklist into memory at startup
    /// Idempotent; call once after Open.
    Status LoadBlacklist();

    /// Remove expired rows from token_blacklist + drop expired in-memory entries
    /// Safe to call periodically.
    Status CleanupExpired();

    /// S7 hook: replace the set of secrets accepted on verify (the dual-key
    /// window = {current, prev}); `sign_secret` becomes the signing key. Not used
    /// in S3 (single secret) — present so S7 wires rotation without an API change.
    void SetAcceptSecrets(std::string sign_secret, std::vector<std::string> accept);

    // ----------------------------- S5 -----------------------------

    /// Request a password reset: if the email exists, store a
    /// 6-digit code (type='password_reset', 15-min TTL) and send it. ALWAYS
    /// returns Ok regardless of whether the email exists (anti-enumeration, §2.6) — a
    /// genuine send/DB error is the only failure surfaced.
    Status RequestPasswordReset(const std::string& email);

    /// Confirm a password reset: validate the code (exists, type,
    /// unused, unexpired), validate the new password complexity, update the hash,
    /// mark the code used, and revoke ALL of the user's refresh tokens (§2.7
    /// side-effect). Errors: CX_ERR_AUTH_INVALID_RESET_CODE (bad/expired/used) /
    /// CX_ERR_INVALID_REQUEST (weak new password).
    Status ConfirmPasswordReset(const std::string& email, const std::string& code,
                                const std::string& new_password);

    /// Verify an email: validate the code (type='email_verify') and set
    /// users.email_verified=1. Idempotent if already verified. Error:
    /// CX_ERR_AUTH_INVALID_RESET_CODE (bad/expired/used code).
    Status VerifyEmail(const std::string& email, const std::string& code);

    /// Issue + store + send a verification code for `email` of `type`
    /// ("password_reset" | "email_verify"). Exposed so Register (S2) can trigger
    /// the initial verify email when email_verification is on (D3.5 wiring).
    Status IssueVerificationCode(const std::string& email, const std::string& type);

    /// Generate a 6-digit numeric code using a uniform CSPRNG draw.
    static std::string GenerateVerificationCode();

private:
    /// SHA-256(token) lowercase hex — the blacklist/refresh-token storage key.
    static std::string HashToken(const std::string& token);

    /// Issue a signed access token for `user` (current tenant_id/role), 9 fields.
    Result<std::string> IssueAccessToken(const std::string& user_id,
                                         const std::string& email,
                                         const std::string& tenant_id,
                                         const std::string& role,
                                         const std::string& sid);

    sqlite3* db_;
    config::AuthConfig config_;
    IPasswordHasher* hasher_;
    std::string sign_secret_;                 ///< current signing key (HS256)
    std::vector<std::string> accept_secrets_; ///< verify candidates (S7 dual-key)

    std::mutex blacklist_mutex_;
    std::unordered_set<std::string> token_blacklist_;  ///< SHA-256(access_token) hex

    /// Resolve the active email sender: the injected one, or a process-wide
    /// NullEmailSender fallback (default).
    IEmailSender* EmailSender();
    IEmailSender* email_sender_;          ///< may be null → NullEmailSender fallback
    NullEmailSender null_email_sender_;   ///< fallback when email_sender_ == nullptr
};

}  // namespace cortrix::auth
