#include <cstdint>
#include "cortrix/auth/jwt_secret_service.h"

#include <sqlite3.h>

#include <openssl/rand.h>

#include <chrono>
#include <cstdlib>
#include <string>

#include "cortrix/auth/auth_error.h"

namespace cortrix::auth {

namespace {

constexpr int kSecretBytes = 64;  // 512-bit HS256 secret

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 16 random bytes → 32-char hex id (auth_secrets.id; ULID/UUID per §3.5, hex is
// equivalent for a unique opaque key). Reuses the GenerateRequestId idiom.
std::string RandomHexId() {
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

// Persist a row as the single status='current' jwt_secret. Caller ensures no
// other current row exists (S1 only ever writes the first one; S7 rotation
// demotes the old current to prev before inserting).
Status InsertCurrentSecret(sqlite3* db, const std::string& id,
                           const std::string& secret) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO auth_secrets(id, secret_type, value, status, created_at, expires_at) "
        "VALUES(?, 'jwt_secret', ?, 'current', ?, NULL)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed,
                          std::string("prepare insert failed: ") + sqlite3_errmsg(db));
    }
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, secret.data(), static_cast<int>(secret.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, NowMs());
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed,
                          std::string("insert step failed: ") + sqlite3_errmsg(db));
    }
    return Status::Ok();
}

}  // namespace

Result<std::string> JwtSecretService::GenerateSecret() {
    std::string secret(kSecretBytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(secret.data()), kSecretBytes) != 1) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed, "RAND_bytes failed");
    }
    return secret;
}

Status JwtSecretService::LoadOrInit() {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed, "platform.db handle is null");
    }

    // (1) env var override — write through so restarts without the env still
    //     load the same secret from platform.db (§2.11 startup behavior step 1).
    if (const char* env = std::getenv("CORTRIX_JWT_SECRET");
        env != nullptr && env[0] != '\0') {
        std::string secret(env);
        // Demote any existing current first so the env value becomes the sole
        // current (idempotent across restarts with the env set).
        if (sqlite3_exec(db_,
                         "UPDATE auth_secrets SET status='retired' "
                         "WHERE secret_type='jwt_secret' AND status='current'",
                         nullptr, nullptr, nullptr) != SQLITE_OK) {
            return AuthStatus(AuthErrorCode::kJwtInitFailed,
                              std::string("env-override demote failed: ") + sqlite3_errmsg(db_));
        }
        Status st = InsertCurrentSecret(db_, RandomHexId(), secret);
        if (!st.ok()) return st;
        current_secret_ = std::move(secret);
        return Status::Ok();
    }

    // (2) load existing status='current' jwt_secret if present.
    sqlite3_stmt* stmt = nullptr;
    const char* sel =
        "SELECT value FROM auth_secrets "
        "WHERE secret_type='jwt_secret' AND status='current' LIMIT 1";
    if (sqlite3_prepare_v2(db_, sel, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed,
                          std::string("prepare select failed: ") + sqlite3_errmsg(db_));
    }
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        const int len = sqlite3_column_bytes(stmt, 0);
        if (blob != nullptr && len > 0) {
            current_secret_.assign(static_cast<const char*>(blob),
                                   static_cast<size_t>(len));
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    if (found) return Status::Ok();

    // (3) none → auto-generate + persist (P08 §2.11 startup behavior step 3; topic 1.1 C).
    Result<std::string> gen = GenerateSecret();
    if (!gen.ok()) return gen.status();
    Status st = InsertCurrentSecret(db_, RandomHexId(), gen.value());
    if (!st.ok()) return st;
    current_secret_ = std::move(gen.value());
    return Status::Ok();
}

// ============================ S7: rotate / window ============================

Result<JwtSecretService::RotationResult> JwtSecretService::RotateJwtSecret() {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed, "platform.db handle is null");
    }
    const int64_t now = NowMs();
    const int64_t prev_expires = now + kPrevWindowMs;

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed, "rotate begin failed");
    }

    // Capture the outgoing current id (for the response) before demoting it.
    std::string prev_id;
    {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db_,
                               "SELECT id FROM auth_secrets WHERE secret_type='jwt_secret' "
                               "AND status='current' LIMIT 1",
                               -1, &q, nullptr) == SQLITE_OK) {
            if (sqlite3_step(q) == SQLITE_ROW) {
                const char* id = reinterpret_cast<const char*>(sqlite3_column_text(q, 0));
                if (id) prev_id = id;
            }
            sqlite3_finalize(q);
        }
    }

    // Retire any existing prev first (only one prev allowed, §3.5), then demote
    // current → prev with a 24h expiry.
    auto fail = [&](const char* msg) -> Status {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kJwtInitFailed, msg);
    };
    if (sqlite3_exec(db_,
                     "UPDATE auth_secrets SET status='retired' "
                     "WHERE secret_type='jwt_secret' AND status='prev'",
                     nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail("retire old prev failed");
    }
    {
        sqlite3_stmt* upd = nullptr;
        if (sqlite3_prepare_v2(db_,
                               "UPDATE auth_secrets SET status='prev', expires_at=? "
                               "WHERE secret_type='jwt_secret' AND status='current'",
                               -1, &upd, nullptr) != SQLITE_OK) {
            return fail("demote current prepare failed");
        }
        sqlite3_bind_int64(upd, 1, prev_expires);
        const int rc = sqlite3_step(upd);
        sqlite3_finalize(upd);
        if (rc != SQLITE_DONE) return fail("demote current failed");
    }

    // Insert the new current.
    Result<std::string> gen = GenerateSecret();
    if (!gen.ok()) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return gen.status();
    }
    const std::string new_id = RandomHexId();
    Status ins = InsertCurrentSecret(db_, new_id, gen.value());
    if (!ins.ok()) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return ins;
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail("rotate commit failed");
    }

    current_secret_ = std::move(gen.value());
    RotationResult out;
    out.new_key_id = new_id;
    out.prev_key_id = prev_id;
    out.prev_expires_at_ms = prev_expires;
    return out;
}

Result<std::vector<std::string>> JwtSecretService::GetAcceptSecrets() {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed, "platform.db handle is null");
    }
    const int64_t now = NowMs();
    std::vector<std::string> out;

    // current first (signing key) then any still-valid prev (dual-key window).
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT value, status, expires_at FROM auth_secrets "
        "WHERE secret_type='jwt_secret' AND status IN ('current','prev') "
        "ORDER BY CASE status WHEN 'current' THEN 0 ELSE 1 END";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed, "accept-secrets prepare failed");
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        const int len = sqlite3_column_bytes(stmt, 0);
        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const bool has_exp = sqlite3_column_type(stmt, 2) != SQLITE_NULL;
        const int64_t exp = has_exp ? sqlite3_column_int64(stmt, 2) : 0;
        if (blob == nullptr || len <= 0) continue;
        // Skip an expired prev (the cron may not have retired it yet).
        if (status && std::string(status) == "prev" && has_exp && exp <= now) continue;
        out.emplace_back(static_cast<const char*>(blob), static_cast<size_t>(len));
    }
    sqlite3_finalize(stmt);
    return out;
}

Result<int> JwtSecretService::CleanupExpiredPrev() {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed, "platform.db handle is null");
    }
    const int64_t now = NowMs();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "UPDATE auth_secrets SET status='retired' "
                           "WHERE secret_type='jwt_secret' AND status='prev' "
                           "AND expires_at IS NOT NULL AND expires_at <= ?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed, "cleanup prev prepare failed");
    }
    sqlite3_bind_int64(stmt, 1, now);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kJwtInitFailed, "cleanup prev failed");
    }
    return sqlite3_changes(db_);
}

}  // namespace cortrix::auth
