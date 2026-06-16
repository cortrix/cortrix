#include <cstdint>
#include "cortrix/auth/bootstrap_handler.h"

#include <sqlite3.h>

#include <openssl/rand.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_error.h"

namespace cortrix::auth {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string RandomHex(int nbytes) {
    std::string buf(nbytes, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(buf.data()), nbytes);
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

Result<bool> BootstrapHandler::IsFirstStart() const {
    if (db_ == nullptr) return AuthStatus(AuthErrorCode::kInternalError, "not initialized");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM users LIMIT 1", -1, &stmt, nullptr)
        != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "users probe prepare failed");
    }
    const bool has_user = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return !has_user;
}

Result<std::string> BootstrapHandler::GenerateToken() {
    Result<bool> first = IsFirstStart();
    if (!first.ok()) return first.status();
    if (!first.value()) {
        return std::string("");  // not first start → no bootstrap token (§S6)
    }
    std::lock_guard<std::mutex> lock(mu_);
    token_ = "ctx_bootstrap_" + RandomHex(24);
    token_expires_ms_ = NowMs() + kTokenTtlMs;
    token_used_ = false;
    return token_;
}

Result<BootstrapHandler::BootstrapResult> BootstrapHandler::Consume(
    const std::string& token) {
    if (db_ == nullptr || api_keys_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "not initialized");
    }

    // Validate the token under the lock; capture used/expiry reasons (§5.1
    // reason: not_found | used | expired). Mark used on success BEFORE the DB work
    // so a concurrent second consume sees it used (single-use, V3 resolution 5).
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (token_.empty() || token != token_) {
            return AuthStatus(AuthErrorCode::kBootstrapTokenInvalid, "reason=not_found");
        }
        if (token_used_) {
            return AuthStatus(AuthErrorCode::kBootstrapTokenInvalid, "reason=used");
        }
        if (NowMs() >= token_expires_ms_) {
            return AuthStatus(AuthErrorCode::kBootstrapTokenInvalid, "reason=expired");
        }
        token_used_ = true;
    }

    // Create the system admin user (P08 §2.13.2 step 3). password_hash is a
    // placeholder sentinel — the admin authenticates via the API Key, not a
    // password (no password login path for the bootstrap admin). tenant/role
    // (Personal Tenant, owner) are filled by P09 at D3.5.
    const std::string user_id = "usr_" + RandomHex(4);
    const int64_t now_sec = NowMs() / 1000;
    sqlite3_stmt* ins = nullptr;
    const char* usql =
        "INSERT INTO users(id, email, password_hash, display_name, email_verified, "
        "status, login_attempts, created_at, updated_at) "
        "VALUES(?, ?, '!bootstrap-no-password', 'System Admin', 1, 'active', 0, ?, ?)";
    if (sqlite3_prepare_v2(db_, usql, -1, &ins, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "admin user insert prepare failed");
    }
    const std::string admin_email = "admin@localhost";
    sqlite3_bind_text(ins, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, admin_email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 3, now_sec);
    sqlite3_bind_int64(ins, 4, now_sec);
    const int rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "admin user insert failed");
    }

    // Mint the admin API Key (name='bootstrap-admin', §3.7) — shown once.
    Result<CreatedApiKey> key = api_keys_->CreateApiKey(user_id, "bootstrap-admin", 0);
    if (!key.ok()) return key.status();

    BootstrapResult out;
    out.admin_api_key = key.value().plaintext;
    out.admin_user_id = user_id;
    out.admin_key_id = key.value().info.id;
    return out;
}

std::string BootstrapHandler::RenderHtml(const std::string& admin_api_key) {
    // Minimal inline-styled page (P08 §2.13.2.a). The key is shown once; the URL
    // is then invalidated. Kept dependency-free (no template engine).
    return
        "<!DOCTYPE html>\n<html>\n<head>\n  <title>Cortrix Admin Setup</title>\n"
        "  <style>body{font-family:system-ui;max-width:680px;margin:3rem auto;padding:0 1rem}"
        "code{display:block;background:#f4f4f5;padding:1rem;border-radius:6px;word-break:break-all}"
        "button{margin-top:.5rem;padding:.5rem 1rem}</style>\n</head>\n<body>\n"
        "  <h1>Cortrix Admin API Key (one-time display)</h1>\n"
        "  <p>This admin API Key has full privileges. Store it safely.</p>\n"
        "  <code id=\"key\">" + admin_api_key + "</code>\n"
        "  <button onclick=\"navigator.clipboard.writeText(document.getElementById('key').textContent)\">"
        "Copy to Clipboard</button>\n"
        "  <p>This URL is now invalidated. It cannot be displayed again.</p>\n"
        "  <h2>Next steps (production environment recommended):</h2>\n"
        "  <ol>\n"
        "    <li>Use this admin API Key to create user-level API Keys "
        "(POST /api/v1/auth/api-keys)</li>\n"
        "    <li>Revoke the admin API Key (DELETE /api/v1/auth/api-keys/{admin_key_id})</li>\n"
        "  </ol>\n</body>\n</html>\n";
}

std::string BootstrapHandler::RenderJson(const std::string& admin_api_key) {
    // P08 §2.13.2.b — {admin_api_key, expires_at:null, scopes:["admin:*"]}.
    nlohmann::json j;
    j["admin_api_key"] = admin_api_key;
    j["expires_at"] = nullptr;
    j["scopes"] = nlohmann::json::array({"admin:*"});
    return j.dump();
}

}  // namespace cortrix::auth
