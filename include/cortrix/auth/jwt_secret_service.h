#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

// Forward-declare the SQLite handle; full <sqlite3.h> is pulled in by the .cpp.
typedef struct sqlite3 sqlite3;

namespace cortrix::auth {

/// JWT signing-secret lifecycle over platform.db `auth_secrets`
/// (Cortrix auto-generate + admin-API rotate + dual-key window).
///
/// S1 scope (this header): startup LoadOrInit + the current-secret accessor.
///   - LoadOrInit(): read the status='current' jwt_secret from platform.db; if
///     none, Cortrix auto-generates a 64-byte random secret and persists it.
///     An optional `CORTRIX_JWT_SECRET` env var overrides + is written through
///     (K8s Secret / docker-compose env advanced path, §2.11 startup behavior 1-3).
/// S7 (this) adds RotateJwtSecret() (current→prev + new current, 24h prev
///   window), GetAcceptSecrets() (the dual-key verify set = current + valid
///   prev) and CleanupExpiredPrev() (the 24h cron). AuthService feeds
///   GetAcceptSecrets() into JwtCodec::Decode so a token signed by either the
///   current or the still-valid prev key verifies during the rotation window.
class JwtSecretService {
public:
    /// `db` is an open platform.db handle whose `auth_secrets` table already
    /// exists (P08AuthSchemaProvider migrated it). The service does not own `db`.
    explicit JwtSecretService(sqlite3* db) : db_(db) {}

    /// Idempotent startup init:
    ///   1) if env `CORTRIX_JWT_SECRET` set → use it + upsert as current;
    ///   2) else if a status='current' jwt_secret row exists → load it;
    ///   3) else auto-generate 64 random bytes → persist as current → use it.
    /// On platform.db write failure returns CX_ERR_AUTH_JWT_INIT_FAILED (a FATAL
    /// startup condition per §5.1 — the caller refuses to start).
    Status LoadOrInit();

    /// The in-use HS256 secret (raw bytes). Valid only after a successful
    /// LoadOrInit(); empty before that.
    const std::string& current_secret() const { return current_secret_; }

    /// True once LoadOrInit() has produced a current secret.
    bool initialized() const { return !current_secret_.empty(); }

    /// Generate a fresh 64-byte (512-bit) random secret (OpenSSL RAND_bytes).
    /// Exposed for tests / S7 rotation; not persisted by this call.
    static Result<std::string> GenerateSecret();

    // ------------------------------- S7 ---------------------------------

    /// Result of a rotation (admin API response).
    struct RotationResult {
        std::string new_key_id;
        std::string prev_key_id;
        int64_t prev_expires_at_ms = 0;  ///< current + 24h
    };

    /// Rotate the JWT secret (admin API `rotate-jwt-secret`):
    /// demote the existing current → prev (expires_at = now + 24h), generate +
    /// persist a new current, and update the in-memory current_secret_. Verifying
    /// continues to accept the prev key until it expires (GetAcceptSecrets()).
    /// Atomic (single tx). On failure returns CX_ERR_AUTH_JWT_INIT_FAILED.
    Result<RotationResult> RotateJwtSecret();

    /// The secrets to accept on verify (dual-key window): the current secret
    /// first, then any status='prev' secret whose expires_at is still in the
    /// future. AuthService passes this to JwtCodec::Decode. Re-reads the db so a
    /// rotation by another worker is picked up.
    Result<std::vector<std::string>> GetAcceptSecrets();

    /// 24h cron: retire status='prev' jwt_secrets whose expires_at has
    /// passed (status → 'retired'). Returns the number retired.
    Result<int> CleanupExpiredPrev();

    /// The 24h dual-key window in ms.
    static constexpr int64_t kPrevWindowMs = 24LL * 60 * 60 * 1000;

private:
    sqlite3* db_;
    std::string current_secret_;
};

}  // namespace cortrix::auth
