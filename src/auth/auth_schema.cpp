#include "cortrix/auth/auth_schema.h"

#include <sqlite3.h>

#include <string>

namespace cortrix::auth {

// platform.db Auth schema — transcribed from P08-auth-system.md §3.1–§3.7.
// One DDL batch so the migrator applies it atomically. 7 tables (v0 4 + v1.0 3).
// IF NOT EXISTS on every object (see auth_schema.h for the platform.db vs
// catalog.db separation + idempotency notes).
const char* const kAuthSchemaSql = R"SQL(
-- ===== v0 base tables (4) =====

-- 1. users (§3.1). platform.db users — richer than catalog.db's relationship
--    users; see auth_schema.h DB-separation note.
CREATE TABLE IF NOT EXISTS users (
    id              TEXT PRIMARY KEY,                  -- "usr_" + 8 hex chars
    email           TEXT NOT NULL UNIQUE,
    password_hash   TEXT NOT NULL,                     -- bcrypt hash
    display_name    TEXT NOT NULL,
    email_verified  INTEGER NOT NULL DEFAULT 0,        -- 0=unverified 1=verified
    role            TEXT NOT NULL DEFAULT 'user',      -- admin | user (§2.13-bis admin/users)
    status          TEXT NOT NULL DEFAULT 'active',    -- active | locked | disabled
    locked_until    INTEGER,                           -- Unix ts, lockout expiry
    login_attempts  INTEGER NOT NULL DEFAULT 0,        -- consecutive failure count
    created_at      INTEGER NOT NULL,                  -- Unix ts
    updated_at      INTEGER NOT NULL                   -- Unix ts
);
CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);

-- 2. refresh_tokens (§3.2).
CREATE TABLE IF NOT EXISTS refresh_tokens (
    jti             TEXT PRIMARY KEY,                  -- refresh token unique id
    user_id         TEXT NOT NULL,
    token_hash      TEXT NOT NULL,                     -- SHA-256(refresh_token)
    expires_at      INTEGER NOT NULL,
    revoked         INTEGER NOT NULL DEFAULT 0,        -- 0=valid 1=revoked
    created_at      INTEGER NOT NULL,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_refresh_tokens_user ON refresh_tokens(user_id);
CREATE INDEX IF NOT EXISTS idx_refresh_tokens_expires ON refresh_tokens(expires_at);

-- 3. verification_codes (§3.3). 6-digit numeric code, valid for 15min.
CREATE TABLE IF NOT EXISTS verification_codes (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    email           TEXT NOT NULL,
    code            TEXT NOT NULL,
    type            TEXT NOT NULL,                     -- email_verify | password_reset
    expires_at      INTEGER NOT NULL,
    used            INTEGER NOT NULL DEFAULT 0,        -- 0=unused 1=used
    created_at      INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_verification_codes_email_type
    ON verification_codes(email, type);

-- 4. token_blacklist (§3.4). Persists revoked access_tokens; loaded into an
--    in-memory set at startup.
CREATE TABLE IF NOT EXISTS token_blacklist (
    token_hash      TEXT PRIMARY KEY,                  -- SHA-256(access_token)
    expires_at      INTEGER NOT NULL,                  -- token's original expiry time
    created_at      INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_token_blacklist_expires
    ON token_blacklist(expires_at);

-- ===== v1.0 new tables (3) =====

-- 5. auth_secrets (§3.5, topic 1.1C). JWT secret auto-generated + persisted +
--    dual-key window.
--    Constraints (§3.5): one status='current' per secret_type; <=1 status='prev'.
--    CHECK(secret_type IN ('jwt_secret','siphash_id_key')) — JWT signing secret +
--    the ID-system SipHash key (16-byte k0||k1) share this table.
CREATE TABLE IF NOT EXISTS auth_secrets (
    id              TEXT PRIMARY KEY,                  -- ULID / UUID
    secret_type     TEXT NOT NULL,                     -- 'jwt_secret'
    value           BLOB NOT NULL,                     -- 64-byte random string (HS256 secret)
    status          TEXT NOT NULL DEFAULT 'current',   -- current | prev | retired
    created_at      INTEGER NOT NULL,                  -- Unix epoch ms
    expires_at      INTEGER,                           -- prev=current+24h; current=null
    CHECK (status IN ('current', 'prev', 'retired')),
    CHECK (secret_type IN ('jwt_secret', 'siphash_id_key'))
);
CREATE INDEX IF NOT EXISTS idx_auth_secrets_type_status
    ON auth_secrets(secret_type, status);

-- 6. auth_config (§3.6, topic 8). Minimal IGlobalConfig persistence (key/value JSON).
CREATE TABLE IF NOT EXISTS auth_config (
    key             TEXT PRIMARY KEY,                  -- "access_token_ttl" / "smtp.host" / ...
    value           TEXT NOT NULL,                     -- JSON (int/string/bool/null)
    updated_at      INTEGER NOT NULL,                  -- Unix epoch ms
    updated_by      TEXT                               -- user_id | 'system'
);

-- 7. api_keys (§3.7, topic 7D). Simplified API Key; key_hash stores no plaintext.
--    Bootstrap admin key name='bootstrap-admin' is stored in the same table.
CREATE TABLE IF NOT EXISTS api_keys (
    id              TEXT PRIMARY KEY,                  -- ULID / UUID (key_id)
    user_id         TEXT NOT NULL,
    name            TEXT NOT NULL,                     -- "my-rag-agent"
    key_hash        TEXT NOT NULL,                     -- SHA-256(cortrix_sk_<random>)
    key_prefix      TEXT NOT NULL,                     -- "cortrix_sk_8f3a..." first 16 chars
    created_at      INTEGER NOT NULL,                  -- Unix epoch ms
    expires_at      INTEGER,                           -- null = never expires
    last_used_at    INTEGER,                           -- most recent successful auth
    revoked_at      INTEGER,                           -- revocation time (non-null when status='revoked')
    status          TEXT NOT NULL DEFAULT 'active',    -- active | revoked | expired
    FOREIGN KEY (user_id) REFERENCES users(id),
    CHECK (status IN ('active', 'revoked', 'expired'))
);
CREATE INDEX IF NOT EXISTS idx_api_keys_user_status ON api_keys(user_id, status);
CREATE INDEX IF NOT EXISTS idx_api_keys_key_hash ON api_keys(key_hash);
)SQL";

Status AuthSchemaProvider::Migrate(sqlite3* db, int /*from_ver*/, int /*to_ver*/) {
    // Phase 1: single-step creation from scratch (v0 → v1). Phase 2 internal
    // evolution will branch on (from_ver, to_ver) here.
    char* err = nullptr;
    if (sqlite3_exec(db, kAuthSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "sqlite error";
        sqlite3_free(err);
        return Status::Internal(std::string("auth schema migration failed: ") + msg);
    }
    return Status::Ok();
}

}  // namespace cortrix::auth
