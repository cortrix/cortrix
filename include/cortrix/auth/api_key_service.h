#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::auth {

/// A stored API key row as surfaced by the list endpoint — never
/// includes the plaintext key (only the prefix for identification).
struct ApiKeyInfo {
    std::string id;            ///< key_id (ULID/UUID)
    std::string user_id;
    std::string name;
    std::string key_prefix;    ///< "cortrix_sk_8f3a..." first 16 chars
    int64_t created_at = 0;    ///< Unix epoch ms
    int64_t expires_at = 0;    ///< 0 = never (NULL in db)
    int64_t last_used_at = 0;  ///< 0 = never used
    std::string status;        ///< active | revoked | expired
};

/// Result of creating a key — the ONLY time the plaintext is returned.
struct CreatedApiKey {
    ApiKeyInfo info;
    std::string plaintext;     ///< "cortrix_sk_<64-char>" — show once, never stored
};

/// Simplified API Key service over platform.db `api_keys`.
/// Keys are `cortrix_sk_<64-char>`; only their SHA-256 hash + a 16-char
/// prefix are persisted (plaintext returned once at creation). Borrows an open
/// platform.db handle (does not own it). The REST routes that expose these are
/// integration wiring; this is the logic they call.
class ApiKeyService {
public:
    explicit ApiKeyService(sqlite3* db) : db_(db) {}

    /// Create a key for `user_id` (POST). `expires_at_ms` = 0 → never
    /// expires. Returns the plaintext ONCE (caller must surface it immediately).
    Result<CreatedApiKey> CreateApiKey(const std::string& user_id,
                                       const std::string& name,
                                       int64_t expires_at_ms = 0);

    /// Validate a presented plaintext key (middleware path): hash →
    /// lookup → check status active + not expired → bump last_used_at → return the
    /// owning user_id. Errors: CX_ERR_AUTH_INVALID_API_KEY (revoked/expired/not_found).
    Result<std::string> ValidateApiKey(const std::string& plaintext);

    /// List a user's keys (GET) — NEVER returns plaintext.
    Result<std::vector<ApiKeyInfo>> ListApiKeys(const std::string& user_id);

    /// Revoke a key by id (DELETE): status='revoked' + revoked_at.
    /// NotFound → CX_ERR_NOT_FOUND. Idempotent on an already-revoked key.
    Status RevokeApiKey(const std::string& key_id);

    /// Generate a fresh "cortrix_sk_<64-char-hex>" plaintext key (OpenSSL random).
    /// Exposed for Bootstrap (the admin key) + tests.
    static std::string GenerateApiKeyPlaintext();

    /// SHA-256(plaintext) lowercase hex — the `api_keys.key_hash` storage key.
    static std::string HashApiKey(const std::string& plaintext);

private:
    sqlite3* db_;
};

}  // namespace cortrix::auth
