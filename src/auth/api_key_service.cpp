#include "cortrix/auth/api_key_service.h"

#include <sqlite3.h>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "cortrix/auth/auth_error.h"

namespace cortrix::auth {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string RandomHex(int nbytes) {
    std::vector<unsigned char> buf(nbytes);
    RAND_bytes(buf.data(), nbytes);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(nbytes * 2);
    for (unsigned char b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

}  // namespace

std::string ApiKeyService::GenerateApiKeyPlaintext() {
    // "cortrix_sk_" + 64 hex chars (32 random bytes). P08 §2.13.2.a / §3.7.
    return "cortrix_sk_" + RandomHex(32);
}

std::string ApiKeyService::HashApiKey(const std::string& plaintext) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size(), digest);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char b : digest) ss << std::setw(2) << static_cast<int>(b);
    return ss.str();
}

Result<CreatedApiKey> ApiKeyService::CreateApiKey(const std::string& user_id,
                                                  const std::string& name,
                                                  int64_t expires_at_ms) {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");

    // CE no-auth / first-start posture (R1, F04 v1.0.4 family): the api_keys FK
    // targets users(id), but with auth disabled no user row exists until the
    // bootstrap URL is consumed. Ensure the owning principal lazily so key
    // minting works in the single-tenant deployment; a real registered user id
    // already exists and the INSERT OR IGNORE is a no-op.
    {
        sqlite3_stmt* ustmt = nullptr;
        const char* usql =
            "INSERT OR IGNORE INTO users (id, email, password_hash, display_name, "
            "email_verified, status, created_at, updated_at) "
            "VALUES (?, ?, '', 'Local admin', 1, 'active', ?, ?)";
        if (sqlite3_prepare_v2(db_, usql, -1, &ustmt, nullptr) == SQLITE_OK) {
            const int64_t unow = NowMs();
            const std::string email = user_id + "@local";
            sqlite3_bind_text(ustmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ustmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ustmt, 3, unow);
            sqlite3_bind_int64(ustmt, 4, unow);
            sqlite3_step(ustmt);
            sqlite3_finalize(ustmt);
        }
    }

    const std::string plaintext = GenerateApiKeyPlaintext();
    const std::string key_hash = HashApiKey(plaintext);
    const std::string key_prefix = plaintext.substr(0, 16);  // "cortrix_sk_<4hex>"
    const std::string id = RandomHex(16);
    const int64_t now = NowMs();

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO api_keys(id, user_id, name, key_hash, key_prefix, created_at, "
        "expires_at, status) VALUES(?, ?, ?, ?, ?, ?, ?, 'active')";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "api_key insert prepare failed");
    }
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, key_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, key_prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, now);
    if (expires_at_ms > 0) {
        sqlite3_bind_int64(stmt, 7, expires_at_ms);
    } else {
        sqlite3_bind_null(stmt, 7);  // never expires
    }
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "api_key insert failed");
    }

    CreatedApiKey out;
    out.info.id = id;
    out.info.user_id = user_id;
    out.info.name = name;
    out.info.key_prefix = key_prefix;
    out.info.created_at = now;
    out.info.expires_at = expires_at_ms;
    out.info.status = "active";
    out.plaintext = plaintext;
    return out;
}

Result<std::string> ApiKeyService::ValidateApiKey(const std::string& plaintext) {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");

    const std::string key_hash = HashApiKey(plaintext);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT user_id, status, expires_at FROM api_keys WHERE key_hash = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "api_key lookup prepare failed");
    }
    sqlite3_bind_text(stmt, 1, key_hash.c_str(), -1, SQLITE_TRANSIENT);

    std::string user_id, status;
    int64_t expires_at = 0;
    bool has_expiry = false, found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        has_expiry = sqlite3_column_type(stmt, 2) != SQLITE_NULL;
        if (has_expiry) expires_at = sqlite3_column_int64(stmt, 2);
    }
    sqlite3_finalize(stmt);

    // §5.1: invalid/revoked/expired → CX_ERR_AUTH_INVALID_API_KEY (reason in
    // structured_data at the API layer: not_found | revoked | expired).
    if (!found) return AuthStatus(AuthErrorCode::kInvalidApiKey, "reason=not_found");
    if (status == "revoked") return AuthStatus(AuthErrorCode::kInvalidApiKey, "reason=revoked");
    if (has_expiry && expires_at <= NowMs()) {
        return AuthStatus(AuthErrorCode::kInvalidApiKey, "reason=expired");
    }

    // Bump last_used_at (best-effort; a failure here does not deny the request).
    sqlite3_exec(db_,
                 ("UPDATE api_keys SET last_used_at=" + std::to_string(NowMs()) +
                  " WHERE key_hash='" + key_hash + "'").c_str(),
                 nullptr, nullptr, nullptr);
    return user_id;
}

Result<std::vector<ApiKeyInfo>> ApiKeyService::ListApiKeys(const std::string& user_id) {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, name, key_prefix, created_at, expires_at, last_used_at, status "
        "FROM api_keys WHERE user_id = ? ORDER BY created_at DESC";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "api_key list prepare failed");
    }
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<ApiKeyInfo> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ApiKeyInfo k;
        k.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        k.user_id = user_id;
        k.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        k.key_prefix = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        k.created_at = sqlite3_column_int64(stmt, 3);
        k.expires_at = sqlite3_column_type(stmt, 4) != SQLITE_NULL
                           ? sqlite3_column_int64(stmt, 4) : 0;
        k.last_used_at = sqlite3_column_type(stmt, 5) != SQLITE_NULL
                             ? sqlite3_column_int64(stmt, 5) : 0;
        k.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        out.push_back(std::move(k));
    }
    sqlite3_finalize(stmt);
    return out;
}

Status ApiKeyService::RevokeApiKey(const std::string& key_id) {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE api_keys SET status='revoked', revoked_at=? "
        "WHERE id=? AND status!='revoked'";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "api_key revoke prepare failed");
    }
    sqlite3_bind_int64(stmt, 1, NowMs());
    sqlite3_bind_text(stmt, 2, key_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "api_key revoke failed");
    }
    // Distinguish "not found" from "already revoked": both leave 0 changes, but
    // only a truly-absent id is an error (idempotent on already-revoked).
    if (sqlite3_changes(db_) == 0) {
        sqlite3_stmt* q = nullptr;
        bool exists = false;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM api_keys WHERE id=?", -1, &q, nullptr)
            == SQLITE_OK) {
            sqlite3_bind_text(q, 1, key_id.c_str(), -1, SQLITE_TRANSIENT);
            exists = (sqlite3_step(q) == SQLITE_ROW);
            sqlite3_finalize(q);
        }
        if (!exists) return AuthStatus(AuthErrorCode::kNotFound, "key_id not found");
    }
    return Status::Ok();
}

}  // namespace cortrix::auth
