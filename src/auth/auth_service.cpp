#include <cstdint>
#include "cortrix/auth/auth_service.h"

#include <sqlite3.h>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/jwt_utils.h"

namespace cortrix::auth {

namespace {

constexpr int kDisplayNameMax = 64;  // P08 §4.1 step 1 (display_name 1..64)

int64_t NowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// "<prefix>_" + 16 random hex chars — for sid ("sess_") / jti ("ref_").
std::string RandomToken(const char* prefix) {
    unsigned char buf[8];
    RAND_bytes(buf, sizeof(buf));
    static const char* kHex = "0123456789abcdef";
    std::string out = prefix;
    for (unsigned char b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

// Does a user with this email already exist? (P08 §4.1 step 2.) Sets *ok=false on
// a query failure (caller maps to a transient/internal error).
bool EmailExists(sqlite3* db, const std::string& email, bool* ok) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE email = ? LIMIT 1", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        *ok = false;
        return false;
    }
    sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
    const bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    *ok = true;
    return exists;
}

}  // namespace

Result<UserInfo> AuthService::Register(const std::string& email,
                                       const std::string& password,
                                       const std::string& display_name) {
    if (db_ == nullptr || hasher_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "AuthService not initialized");
    }

    // 1) Parameter validation (P08 §4.1 step 1). Distinct rules → CX_ERR_INVALID_REQUEST
    //    with the violated rule(s) in structured_data (§5.2 { rules_violated }).
    nlohmann::json rules = nlohmann::json::array();
    if (!ValidateEmail(email)) rules.push_back("email_format");
    if (!ValidatePassword(password, config_.password_min_length))
        rules.push_back("password_complexity");
    if (display_name.empty() || display_name.size() > kDisplayNameMax)
        rules.push_back("display_name_length");
    if (!rules.empty()) {
        return AuthStatus(AuthErrorCode::kInvalidRequest,
                          "registration validation failed: " + rules.dump());
    }

    // 2) Email uniqueness pre-check (the UNIQUE constraint is the real guarantee
    //    under concurrency — step 3 maps a constraint violation to the same code).
    bool query_ok = false;
    if (EmailExists(db_, email, &query_ok)) {
        return AuthStatus(AuthErrorCode::kEmailAlreadyExists, email);
    }
    if (!query_ok) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "users lookup failed");
    }

    // 3) Hash password + INSERT user, inside a transaction. The transaction seam
    //    is where P09 TenantService::CreatePersonal(user_id, email) joins as
    //    step 4 (atomic with the users INSERT, P08 §4.1 / §"P09 dependency notes"):
    //    P08 holds the outermost tx; P09 reuses this same connection. That is
    //    cross-Feature wiring → 🚩 D3.5 (P09 has no code yet). Standalone S2
    //    commits the users row alone; the BEGIN/COMMIT is already here so wiring
    //    P09 in later is a localized insert, not a restructure.
    Result<std::string> hash = hasher_->Hash(password);
    if (!hash.ok()) return hash.status();

    const std::string user_id = GenerateUserId();
    const int64_t now = NowSec();

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "begin tx failed");
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO users(id, email, password_hash, display_name, email_verified, "
        "status, login_attempts, created_at, updated_at) "
        "VALUES(?, ?, ?, ?, 0, 'active', 0, ?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "users insert prepare failed");
    }
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, hash.value().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_bind_int64(stmt, 6, now);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        // UNIQUE(email) violation under concurrency → the registration race lost
        // (P08 §5.1 "concurrent registration of the same email" → 409 same as the pre-check).
        if (rc == SQLITE_CONSTRAINT) {
            return AuthStatus(AuthErrorCode::kEmailAlreadyExists, email);
        }
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "users insert failed");
    }

    // (4) → D3.5: P09 CreatePersonal here, in this same tx.
    // (5) email verification: only when config_.email_verification (default
    //     false). The send path is S5; standalone S2 leaves it off.

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "commit failed");
    }

    UserInfo info;
    info.id = user_id;
    info.email = email;
    info.display_name = display_name;
    info.email_verified = false;
    info.created_at = now;
    // info.tenants stays empty until the P09 wiring (D3.5).
    return info;
}

Result<UserInfo> AuthService::GetUserInfo(const std::string& user_id) {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "AuthService not initialized");
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT email, display_name, email_verified, created_at "
        "FROM users WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user lookup prepare failed");
    }
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);

    UserInfo info;
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        info.id = user_id;
        const char* em = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* dn = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        info.email = em ? em : "";
        info.display_name = dn ? dn : "";
        info.email_verified = sqlite3_column_int(stmt, 2) != 0;
        info.created_at = sqlite3_column_int64(stmt, 3);
    }
    sqlite3_finalize(stmt);

    if (!found) {
        return AuthStatus(AuthErrorCode::kUserNotFound, user_id);
    }
    return info;
}

// ============================== S3: JWT / login ==============================

std::string AuthService::HashToken(const std::string& token) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), digest);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char b : digest) ss << std::setw(2) << static_cast<int>(b);
    return ss.str();
}

void AuthService::SetAcceptSecrets(std::string sign_secret,
                                   std::vector<std::string> accept) {
    sign_secret_ = std::move(sign_secret);
    accept_secrets_ = std::move(accept);
}

Result<std::string> AuthService::IssueAccessToken(const std::string& user_id,
                                                  const std::string& email,
                                                  const std::string& tenant_id,
                                                  const std::string& role,
                                                  const std::string& sid) {
    JwtPayload p;
    p.sub = user_id;
    p.email = email;
    p.tenant_id = tenant_id;
    p.role = role;
    p.sid = sid;
    p.iat = NowSec();
    p.exp = p.iat + config_.access_token_ttl;
    // iss/aud keep their struct defaults ("cortrix-auth" / "cortrix", topic 3.3).
    return JwtCodec::Encode(p, sign_secret_);
}

Result<AuthTokenPair> AuthService::Login(const std::string& email,
                                         const std::string& password) {
    if (db_ == nullptr || hasher_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "AuthService not initialized");
    }

    // 1) look up user (P08 §4.2 step 1). Nonexistent email → same error as wrong
    //    password (anti-enumeration, §5.1).
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, password_hash, status, locked_until, login_attempts "
        "FROM users WHERE email = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "login lookup prepare failed");
    }
    sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);

    std::string user_id, password_hash, status;
    int64_t locked_until = 0;
    int attempts = 0;
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        password_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        locked_until = sqlite3_column_int64(stmt, 3);
        attempts = sqlite3_column_int(stmt, 4);
    }
    sqlite3_finalize(stmt);

    if (!found) {
        return AuthStatus(AuthErrorCode::kInvalidCredentials, "no such user");
    }

    const int64_t now = NowSec();

    // 2) account status / lockout (P08 §4.2 step 2).
    if (status == "disabled") {
        return AuthStatus(AuthErrorCode::kAccountDisabled, "account disabled");
    }
    if (status == "locked") {
        if (locked_until > now) {
            // Still locked → CX_ERR_ACCOUNT_LOCKED with the live remaining ms is
            // surfaced by the API layer via MakeAuthError(retry_after_override);
            // here we carry it in the Status message for the boundary to re-inflate.
            const int64_t remaining_ms = (locked_until - now) * 1000;
            return AuthStatus(AuthErrorCode::kAccountLocked,
                              "locked_until=" + std::to_string(locked_until) +
                                  " remaining_ms=" + std::to_string(remaining_ms));
        }
        // lock expired → auto-unlock (status active, attempts 0) then continue.
        sqlite3_exec(db_,
                     ("UPDATE users SET status='active', login_attempts=0, "
                      "locked_until=NULL, updated_at=" + std::to_string(now) +
                      " WHERE id='" + user_id + "'").c_str(),
                     nullptr, nullptr, nullptr);
        attempts = 0;
    }

    // 3) verify password (P08 §4.2 step 3).
    Result<bool> verify = hasher_->Verify(password, password_hash);
    if (!verify.ok()) return verify.status();
    if (!verify.value()) {
        // bump attempts; lock at the threshold.
        const int new_attempts = attempts + 1;
        if (new_attempts >= config_.max_login_attempts) {
            const int64_t lock_until = now + config_.lockout_duration;
            sqlite3_exec(db_,
                         ("UPDATE users SET status='locked', login_attempts=" +
                          std::to_string(new_attempts) + ", locked_until=" +
                          std::to_string(lock_until) + ", updated_at=" +
                          std::to_string(now) + " WHERE id='" + user_id + "'").c_str(),
                         nullptr, nullptr, nullptr);
        } else {
            sqlite3_exec(db_,
                         ("UPDATE users SET login_attempts=" +
                          std::to_string(new_attempts) + ", updated_at=" +
                          std::to_string(now) + " WHERE id='" + user_id + "'").c_str(),
                         nullptr, nullptr, nullptr);
        }
        return AuthStatus(AuthErrorCode::kInvalidCredentials, "wrong password");
    }

    // 4) success: reset attempts, issue tokens, persist refresh hash (P08 §4.2 step 4).
    //    tenant_id/role come from P09 at D3.5; standalone S3 issues with the
    //    Personal-Tenant placeholder ("" tenant, "owner" role) so the JWT is
    //    well-formed and the flow is testable. 🚩 D3.5: fill from P09.
    sqlite3_exec(db_,
                 ("UPDATE users SET login_attempts=0, locked_until=NULL, updated_at=" +
                  std::to_string(now) + " WHERE id='" + user_id + "'").c_str(),
                 nullptr, nullptr, nullptr);

    const std::string sid = RandomToken("sess_");
    const std::string tenant_id;          // 🚩 D3.5 (P09 active tenant)
    const std::string role = "owner";     // 🚩 D3.5 (P09 membership role)

    Result<std::string> access = IssueAccessToken(user_id, email, tenant_id, role, sid);
    if (!access.ok()) return access.status();

    // refresh token.
    JwtPayload rp;
    rp.sub = user_id;
    rp.sid = sid;
    rp.type = "refresh";
    rp.jti = RandomToken("ref_");
    rp.iat = now;
    rp.exp = now + config_.refresh_token_ttl;
    Result<std::string> refresh = JwtCodec::Encode(rp, sign_secret_);
    if (!refresh.ok()) return refresh.status();

    // persist refresh_token hash (P08 §3.2 / §4.2 step 4).
    sqlite3_stmt* ins = nullptr;
    const char* rsql =
        "INSERT INTO refresh_tokens(jti, user_id, token_hash, expires_at, revoked, created_at) "
        "VALUES(?, ?, ?, ?, 0, ?)";
    if (sqlite3_prepare_v2(db_, rsql, -1, &ins, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "refresh insert prepare failed");
    }
    const std::string rhash = HashToken(refresh.value());
    sqlite3_bind_text(ins, 1, rp.jti.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, rhash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 4, rp.exp);
    sqlite3_bind_int64(ins, 5, now);
    const int rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "refresh insert failed");
    }

    AuthTokenPair pair;
    pair.access_token = access.value();
    pair.refresh_token = refresh.value();
    pair.expires_in = config_.access_token_ttl;
    return pair;
}

Status AuthService::Logout(const std::string& access_token) {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");

    // Decode best-effort to recover exp + sid; a bad token is an idempotent no-op
    // success (logging out an already-invalid token must not error, §4.7).
    Result<JwtPayload> decoded = JwtCodec::Decode(access_token, accept_secrets_, NowSec());
    if (!decoded.ok()) {
        return Status::Ok();  // nothing to revoke
    }
    const JwtPayload& p = decoded.value();
    const int64_t now = NowSec();

    // 1) blacklist the access token (memory + persist, §4.7 step 3).
    const std::string thash = HashToken(access_token);
    {
        std::lock_guard<std::mutex> lock(blacklist_mutex_);
        token_blacklist_.insert(thash);
    }
    sqlite3_stmt* ins = nullptr;
    const char* bsql =
        "INSERT OR IGNORE INTO token_blacklist(token_hash, expires_at, created_at) "
        "VALUES(?, ?, ?)";
    if (sqlite3_prepare_v2(db_, bsql, -1, &ins, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, thash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins, 2, p.exp);
        sqlite3_bind_int64(ins, 3, now);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
    }

    // 2) revoke the refresh token(s) for this session (§4.7 step 4). Logout is
    //    per-session (the sid) so other devices keep working (§2.10 sid note).
    if (!p.sid.empty()) {
        sqlite3_stmt* upd = nullptr;
        const char* usql =
            "UPDATE refresh_tokens SET revoked=1 WHERE user_id=? AND revoked=0 "
            "AND jti IN (SELECT jti FROM refresh_tokens WHERE user_id=?)";
        // Note: refresh tokens don't store sid; revoke this user's active refresh
        // tokens. (Per-sid refresh revocation refinement is fine to tighten later;
        // single-session V1 behavior matches §4.7 "revoke associated refresh".)
        if (sqlite3_prepare_v2(db_, usql, -1, &upd, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(upd, 1, p.sub.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upd, 2, p.sub.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(upd);
            sqlite3_finalize(upd);
        }
    }
    return Status::Ok();
}

Result<std::string> AuthService::RefreshAccessToken(const std::string& refresh_token) {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");

    const int64_t now = NowSec();

    // 1) decode + verify signature/expiry (P08 §4.4 step 1). A bad/expired token
    //    maps to the refresh-specific codes.
    Result<JwtPayload> decoded = JwtCodec::Decode(refresh_token, accept_secrets_, now);
    if (!decoded.ok()) {
        // JwtCodec returns kTokenExpired for expiry, kUnauthorized otherwise; remap
        // the generic case to the refresh-token-specific code (§2.5).
        if (decoded.status().code() == StatusCode::kUnauthenticated &&
            decoded.status().message().find("CX_ERR_AUTH_TOKEN_EXPIRED") == std::string::npos) {
            return AuthStatus(AuthErrorCode::kInvalidRefreshToken, "decode failed");
        }
        return decoded.status();  // already CX_ERR_AUTH_TOKEN_EXPIRED
    }
    const JwtPayload& p = decoded.value();

    // 2) must be a refresh token (P08 §4.4 step 2).
    if (!p.is_refresh()) {
        return AuthStatus(AuthErrorCode::kInvalidRefreshToken, "not a refresh token");
    }

    // 3) look up the refresh_tokens row by jti (P08 §4.4 step 3-4).
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT token_hash, expires_at, revoked FROM refresh_tokens WHERE jti = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "refresh lookup prepare failed");
    }
    sqlite3_bind_text(stmt, 1, p.jti.c_str(), -1, SQLITE_TRANSIENT);
    std::string stored_hash;
    int64_t expires_at = 0;
    int revoked = 0;
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        stored_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        expires_at = sqlite3_column_int64(stmt, 1);
        revoked = sqlite3_column_int(stmt, 2);
    }
    sqlite3_finalize(stmt);

    if (!found) return AuthStatus(AuthErrorCode::kInvalidRefreshToken, "jti not found");
    if (revoked != 0) return AuthStatus(AuthErrorCode::kTokenRevoked, "refresh revoked");
    if (expires_at <= now) return AuthStatus(AuthErrorCode::kTokenExpired, "refresh expired");

    // 4) token hash must match (P08 §4.4 step 4 — defends against a forged jti).
    if (HashToken(refresh_token) != stored_hash) {
        return AuthStatus(AuthErrorCode::kInvalidRefreshToken, "hash mismatch");
    }

    // 5) user still active (P08 §4.4 step 5) + read CURRENT email/tenant/role.
    sqlite3_stmt* us = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT email, status FROM users WHERE id = ?", -1,
                           &us, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user status prepare failed");
    }
    sqlite3_bind_text(us, 1, p.sub.c_str(), -1, SQLITE_TRANSIENT);
    std::string email, ustatus;
    bool ufound = false;
    if (sqlite3_step(us) == SQLITE_ROW) {
        ufound = true;
        email = reinterpret_cast<const char*>(sqlite3_column_text(us, 0));
        ustatus = reinterpret_cast<const char*>(sqlite3_column_text(us, 1));
    }
    sqlite3_finalize(us);
    if (!ufound || ustatus != "active") {
        return AuthStatus(AuthErrorCode::kAccountDisabled, "user not active");
    }

    // 6) issue a new access token reusing the same sid (P08 §4.4 step 6). tenant/
    //    role from P09 at D3.5 (placeholder here, same as Login).
    return IssueAccessToken(p.sub, email, /*tenant_id=*/"", /*role=*/"owner", p.sid);
}

Result<AuthContext> AuthService::ValidateAccessToken(const std::string& access_token) {
    const int64_t now = NowSec();

    // 1-2) decode + verify signature + expiry (P08 §4.3 step 2).
    Result<JwtPayload> decoded = JwtCodec::Decode(access_token, accept_secrets_, now);
    if (!decoded.ok()) return decoded.status();  // kUnauthorized / kTokenExpired
    const JwtPayload& p = decoded.value();

    // type must be access (a refresh token is not valid here, §4.3 step 2).
    if (p.is_refresh()) {
        return AuthStatus(AuthErrorCode::kUnauthorized, "refresh token used as access");
    }

    // blacklist check (§4.3 step 2 — O(1) memory set).
    {
        std::lock_guard<std::mutex> lock(blacklist_mutex_);
        if (token_blacklist_.count(HashToken(access_token)) != 0) {
            return AuthStatus(AuthErrorCode::kTokenRevoked, "token blacklisted");
        }
    }

    AuthContext ctx;
    ctx.user_id = p.sub;
    ctx.email = p.email;
    ctx.tenant_id = p.tenant_id;
    ctx.role = p.role;
    ctx.session_id = p.sid;
    return ctx;
}

Status AuthService::LoadBlacklist() {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");
    const int64_t now = NowSec();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT token_hash FROM token_blacklist WHERE expires_at > ?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "blacklist load prepare failed");
    }
    sqlite3_bind_int64(stmt, 1, now);
    std::lock_guard<std::mutex> lock(blacklist_mutex_);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* h = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (h != nullptr) token_blacklist_.insert(h);
    }
    sqlite3_finalize(stmt);
    return Status::Ok();
}

Status AuthService::CleanupExpired() {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");
    const int64_t now = NowSec();
    // Persisted blacklist + refresh tokens + verification codes (P08 §3.9).
    sqlite3_exec(db_,
                 ("DELETE FROM token_blacklist WHERE expires_at < " + std::to_string(now))
                     .c_str(),
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_,
                 ("DELETE FROM refresh_tokens WHERE expires_at < " + std::to_string(now))
                     .c_str(),
                 nullptr, nullptr, nullptr);
    // In-memory: drop entries whose persisted row is now gone (expired) by
    // rebuilding the set from the just-cleaned table.
    {
        std::lock_guard<std::mutex> lock(blacklist_mutex_);
        token_blacklist_.clear();
    }
    return LoadBlacklist();
}

// ====================== S5: reset / verify-email / codes =====================

IEmailSender* AuthService::EmailSender() {
    return email_sender_ != nullptr ? email_sender_ : &null_email_sender_;
}

std::string AuthService::GenerateVerificationCode() {
    // 6-digit numeric code (P08 §4.6). Draw a uniform uint32 and reject the tail
    // that would bias the modulo (rejection sampling over [0, floor(2^32/1e6)*1e6)).
    constexpr uint32_t kRange = 1000000;
    constexpr uint32_t kLimit = (0xFFFFFFFFu / kRange) * kRange;
    uint32_t v = 0;
    do {
        RAND_bytes(reinterpret_cast<unsigned char*>(&v), sizeof(v));
    } while (v >= kLimit);
    const uint32_t code = v % kRange;
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%06u", code);
    return std::string(buf, 6);
}

Status AuthService::IssueVerificationCode(const std::string& email,
                                          const std::string& type) {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");
    const int64_t now = NowSec();
    // 15-min TTL (P08 §3.3 / §2.7). Code stored plaintext (short-lived, single-use).
    const std::string code = GenerateVerificationCode();
    const int64_t expires_at = now + 15 * 60;

    sqlite3_stmt* ins = nullptr;
    const char* sql =
        "INSERT INTO verification_codes(email, code, type, expires_at, used, created_at) "
        "VALUES(?, ?, ?, ?, 0, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &ins, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "code insert prepare failed");
    }
    sqlite3_bind_text(ins, 1, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 4, expires_at);
    sqlite3_bind_int64(ins, 5, now);
    const int rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "code insert failed");
    }

    const std::string subject =
        (type == "password_reset") ? "Cortrix password reset code" : "Cortrix email verification code";
    Status sent = EmailSender()->Send(email, subject, "Your code is: " + code);
    if (!sent.ok()) {
        return AuthStatus(AuthErrorCode::kEmailSendFailed, sent.message());
    }
    return Status::Ok();
}

Status AuthService::RequestPasswordReset(const std::string& email) {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");
    // anti-enumeration (P08 §2.6): only issue a code if the email exists, but ALWAYS return
    // Ok so the caller's response is identical regardless of existence.
    bool query_ok = false;
    const bool exists = EmailExists(db_, email, &query_ok);
    if (!query_ok) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user lookup failed");
    }
    if (exists) {
        Status st = IssueVerificationCode(email, "password_reset");
        if (!st.ok()) return st;  // a real send/db error is surfaced (§5.1)
    }
    return Status::Ok();
}

namespace {
// Validate the newest matching code for (email, type): exists, unused, unexpired.
// Returns the row id to mark used on success; -1 + error Status otherwise.
Status CheckVerificationCode(sqlite3* db, const std::string& email,
                             const std::string& type, const std::string& code,
                             int64_t now, int64_t* out_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, expires_at, used FROM verification_codes "
        "WHERE email = ? AND type = ? AND code = ? "
        "ORDER BY created_at DESC LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "code lookup prepare failed");
    }
    sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, code.c_str(), -1, SQLITE_TRANSIENT);
    int64_t id = -1, expires_at = 0;
    int used = 0;
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        id = sqlite3_column_int64(stmt, 0);
        expires_at = sqlite3_column_int64(stmt, 1);
        used = sqlite3_column_int(stmt, 2);
    }
    sqlite3_finalize(stmt);

    // All three failure modes collapse to one code (§5.1: expired/wrong/reused →
    // CX_ERR_AUTH_INVALID_RESET_CODE; reason in structured_data at the API layer).
    if (!found) return AuthStatus(AuthErrorCode::kInvalidResetCode, "reason=invalid");
    if (used != 0) return AuthStatus(AuthErrorCode::kInvalidResetCode, "reason=reused");
    if (expires_at <= now) return AuthStatus(AuthErrorCode::kInvalidResetCode, "reason=expired");
    *out_id = id;
    return Status::Ok();
}

void MarkCodeUsed(sqlite3* db, int64_t id) {
    sqlite3_exec(db,
                 ("UPDATE verification_codes SET used=1 WHERE id=" + std::to_string(id)).c_str(),
                 nullptr, nullptr, nullptr);
}
}  // namespace

Status AuthService::ConfirmPasswordReset(const std::string& email,
                                         const std::string& code,
                                         const std::string& new_password) {
    if (db_ == nullptr || hasher_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "not initialized");
    }
    // New password complexity first (P08 §2.7 → CX_ERR_INVALID_REQUEST).
    if (!ValidatePassword(new_password, config_.password_min_length)) {
        return AuthStatus(AuthErrorCode::kInvalidRequest, "rules_violated=password_complexity");
    }
    const int64_t now = NowSec();
    int64_t code_id = -1;
    Status check = CheckVerificationCode(db_, email, "password_reset", code, now, &code_id);
    if (!check.ok()) return check;

    Result<std::string> hash = hasher_->Hash(new_password);
    if (!hash.ok()) return hash.status();

    // Atomic: update hash + mark code used + revoke all refresh tokens (§2.7
    // side-effect — all issued refresh_tokens for the user are invalidated).
    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "begin tx failed");
    }
    sqlite3_stmt* upd = nullptr;
    const char* usql =
        "UPDATE users SET password_hash=?, updated_at=?, login_attempts=0, "
        "status='active', locked_until=NULL WHERE email=?";
    if (sqlite3_prepare_v2(db_, usql, -1, &upd, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "password update prepare failed");
    }
    sqlite3_bind_text(upd, 1, hash.value().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, now);
    sqlite3_bind_text(upd, 3, email.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "password update failed");
    }
    MarkCodeUsed(db_, code_id);
    // Revoke all of this user's refresh tokens (§2.7 side effect).
    sqlite3_exec(db_,
                 ("UPDATE refresh_tokens SET revoked=1 WHERE revoked=0 AND user_id="
                  "(SELECT id FROM users WHERE email='" + email + "')").c_str(),
                 nullptr, nullptr, nullptr);
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "commit failed");
    }
    return Status::Ok();
}

Status AuthService::VerifyEmail(const std::string& email, const std::string& code) {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");

    // Idempotent: if already verified, succeed without requiring a code (§2.8).
    {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT email_verified FROM users WHERE email=?", -1,
                               &q, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, email.c_str(), -1, SQLITE_TRANSIENT);
            bool already = false;
            if (sqlite3_step(q) == SQLITE_ROW) already = sqlite3_column_int(q, 0) != 0;
            sqlite3_finalize(q);
            if (already) return Status::Ok();
        }
    }

    const int64_t now = NowSec();
    int64_t code_id = -1;
    Status check = CheckVerificationCode(db_, email, "email_verify", code, now, &code_id);
    if (!check.ok()) return check;

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "begin tx failed");
    }
    sqlite3_stmt* upd = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "UPDATE users SET email_verified=1, updated_at=? WHERE email=?",
                           -1, &upd, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "verify update prepare failed");
    }
    sqlite3_bind_int64(upd, 1, now);
    sqlite3_bind_text(upd, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "verify update failed");
    }
    MarkCodeUsed(db_, code_id);
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "commit failed");
    }
    return Status::Ok();
}

}  // namespace cortrix::auth
