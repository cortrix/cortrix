#pragma once
#include <string>

namespace cortrix::config {

/// Runtime Auth configuration (the minimal IGlobalConfig surface).
///
/// v1.0 deliberately does NOT read a config.yaml: every value is sourced from
/// the platform.db `auth_config` table (AuthConfigService::LoadOrInitDefaults
/// writes the defaults on first start; the SMTP-related keys are later set via
/// the admin API `POST /api/v1/admin/config/smtp`). The single startup-required
/// env var is `CORTRIX_DATA_DIR`. The fields below mirror the default key
/// set 1:1 and are the in-memory snapshot AuthService reads.
///
/// This is the minimal form of the canonical cortrix::IGlobalConfig
/// (common/i_global_config.h). This ships the POD snapshot + the table
/// load/init; the OnChange hot-reload wiring + admin-API setters land in S5/S7.
struct AuthConfig {
    // --- Category 2: algorithm parameters (topic 8 — defaults suffice; Phase 2 may evaluate opening an admin API) ---
    int  access_token_ttl   = 3600;     ///< seconds, 1h          (auth_config key)
    int  refresh_token_ttl  = 2592000;  ///< seconds, 30d
    int  bcrypt_cost        = 12;        ///< topic 2
    int  max_login_attempts = 5;
    int  lockout_duration   = 900;       ///< seconds, 15min
    int  password_min_length = 8;        ///< topic 2

    // --- Category 3: external dependency — SMTP (topic 8 — configured via admin API) ---
    bool email_verification = false;     ///< Off by default (avoids depending on SMTP)
    std::string smtp_host;               ///< null/empty ⇒ NullEmailSender behavior
    int  smtp_port          = 587;
    std::string smtp_user;
    std::string smtp_pass;               ///< Masked as "***" when returned by the GET admin API
    bool smtp_tls           = true;

    // JWT algorithm is fixed HS256 in Phase 1; the secret itself is
    // NOT in this struct — it lives in auth_secrets and is owned by
    // JwtSecretService (topic 1.1 C), not auth_config.
    std::string jwt_algorithm = "HS256";
};

/// The platform.db `auth_config` table key names. Centralized so the
/// loader, the admin-API setter (S7) and tests never spell a key as a bare
/// literal. SMTP keys use the dotted form the spec mandates ("smtp.host" …).
namespace auth_config_keys {
constexpr const char* kAccessTokenTtl    = "access_token_ttl";
constexpr const char* kRefreshTokenTtl   = "refresh_token_ttl";
constexpr const char* kBcryptCost        = "bcrypt_cost";
constexpr const char* kMaxLoginAttempts  = "max_login_attempts";
constexpr const char* kLockoutDuration   = "lockout_duration";
constexpr const char* kPasswordMinLength = "password_min_length";
constexpr const char* kEmailVerification = "email_verification";
constexpr const char* kSmtpHost          = "smtp.host";
constexpr const char* kSmtpPort          = "smtp.port";
constexpr const char* kSmtpUser          = "smtp.user";
constexpr const char* kSmtpPass          = "smtp.pass";
constexpr const char* kSmtpTls           = "smtp.tls";
}  // namespace auth_config_keys

/// Number of default keys written on first start (the default key set = 12;
/// the spec's prose "13 items" counts these 12 rows — `smtp` is a YAML grouping
/// header, not a stored row). Compile-time anchor for the S1 default-write test.
constexpr int kAuthConfigDefaultKeyCount = 12;

}  // namespace cortrix::config
