#include <cstdint>
#include "cortrix/auth/auth_config_service.h"

#include <sqlite3.h>

#include <chrono>
#include <utility>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_error.h"

namespace cortrix::auth {

using config::AuthConfig;
namespace keys = config::auth_config_keys;

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The §3.6 default key set, as (key, JSON literal) pairs. JSON literals match the
// spec's types (int / bool / null / string). kAuthConfigDefaultKeyCount anchors
// the count in a test.
struct DefaultKv { const char* key; const char* json; };
const DefaultKv kDefaults[] = {
    {keys::kAccessTokenTtl,    "3600"},
    {keys::kRefreshTokenTtl,   "2592000"},
    {keys::kBcryptCost,        "12"},
    {keys::kMaxLoginAttempts,  "5"},
    {keys::kLockoutDuration,   "900"},
    {keys::kPasswordMinLength, "8"},
    {keys::kEmailVerification, "false"},
    {keys::kSmtpHost,          "null"},
    {keys::kSmtpPort,          "587"},
    {keys::kSmtpUser,          "null"},
    {keys::kSmtpPass,          "null"},
    {keys::kSmtpTls,           "true"},
};

bool TableIsEmpty(sqlite3* db, bool* ok) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM auth_config", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        *ok = false;
        return false;
    }
    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    *ok = (count >= 0);
    return count == 0;
}

Status InsertDefault(sqlite3* db, const char* key, const char* json) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO auth_config(key, value, updated_at, updated_by) "
        "VALUES(?, ?, ?, 'system')";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kInternalError,
                          std::string("auth_config insert prepare failed: ") + sqlite3_errmsg(db));
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, NowMs());
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kInternalError,
                          std::string("auth_config insert failed: ") + sqlite3_errmsg(db));
    }
    return Status::Ok();
}

}  // namespace

void AuthConfigService::ApplyRow(const std::string& key,
                                 const std::string& json_value) {
    // Parse the stored JSON; a malformed row is skipped (defensive — a corrupt
    // value must not crash startup, it just keeps the struct default).
    nlohmann::json v;
    try {
        v = nlohmann::json::parse(json_value);
    } catch (const nlohmann::json::exception&) {
        return;
    }

    auto as_int = [&](int& dst) { if (v.is_number()) dst = v.get<int>(); };
    auto as_bool = [&](bool& dst) { if (v.is_boolean()) dst = v.get<bool>(); };
    auto as_str = [&](std::string& dst) {
        if (v.is_string()) dst = v.get<std::string>();
        else if (v.is_null()) dst.clear();
    };

    if (key == keys::kAccessTokenTtl)         as_int(config_.access_token_ttl);
    else if (key == keys::kRefreshTokenTtl)   as_int(config_.refresh_token_ttl);
    else if (key == keys::kBcryptCost)        as_int(config_.bcrypt_cost);
    else if (key == keys::kMaxLoginAttempts)  as_int(config_.max_login_attempts);
    else if (key == keys::kLockoutDuration)   as_int(config_.lockout_duration);
    else if (key == keys::kPasswordMinLength) as_int(config_.password_min_length);
    else if (key == keys::kEmailVerification) as_bool(config_.email_verification);
    else if (key == keys::kSmtpHost)          as_str(config_.smtp_host);
    else if (key == keys::kSmtpPort)          as_int(config_.smtp_port);
    else if (key == keys::kSmtpUser)          as_str(config_.smtp_user);
    else if (key == keys::kSmtpPass)          as_str(config_.smtp_pass);
    else if (key == keys::kSmtpTls)           as_bool(config_.smtp_tls);
    // else: unknown key (forward-compatible) — ignored.
}

Status AuthConfigService::LoadOrInitDefaults() {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "platform.db handle is null");
    }

    std::lock_guard<std::mutex> lock(mu_);

    bool count_ok = false;
    const bool empty = TableIsEmpty(db_, &count_ok);
    if (!count_ok) {
        return AuthStatus(AuthErrorCode::kInternalError,
                          std::string("auth_config count failed: ") + sqlite3_errmsg(db_));
    }

    // First start: write defaults in one transaction (all-or-nothing).
    if (empty) {
        if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return AuthStatus(AuthErrorCode::kInternalError, "auth_config BEGIN failed");
        }
        for (const DefaultKv& kv : kDefaults) {
            Status st = InsertDefault(db_, kv.key, kv.json);
            if (!st.ok()) {
                sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                return st;
            }
        }
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return AuthStatus(AuthErrorCode::kInternalError, "auth_config COMMIT failed");
        }
    }

    // Load every row into the in-memory snapshot (covers both the fresh-default
    // and the already-populated paths).
    config_ = AuthConfig{};  // reset to struct defaults before applying rows
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT key, value FROM auth_config", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kInternalError,
                          std::string("auth_config select failed: ") + sqlite3_errmsg(db_));
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (k != nullptr && val != nullptr) ApplyRow(k, val);
    }
    sqlite3_finalize(stmt);
    return Status::Ok();
}

AuthConfig AuthConfigService::Get() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_;
}

void AuthConfigService::OnChange(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lock(mu_);
    on_change_.push_back(std::move(cb));
}

Status AuthConfigService::ReloadKey(const std::string& key) {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "platform.db handle is null");
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT value FROM auth_config WHERE key = ?", -1,
                               &stmt, nullptr) != SQLITE_OK) {
            return AuthStatus(AuthErrorCode::kInternalError,
                              std::string("auth_config reload prepare failed: ") + sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        bool found = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (val != nullptr) {
                ApplyRow(key, val);
                found = true;
            }
        }
        sqlite3_finalize(stmt);
        if (!found) {
            return AuthStatus(AuthErrorCode::kNotFound,
                              std::string("auth_config key not found: ") + key);
        }
    }
    NotifyChange(key);  // outside the lock (callbacks may call back into Get())
    return Status::Ok();
}

void AuthConfigService::NotifyChange(const std::string& key) {
    std::vector<std::function<void(const std::string&)>> cbs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cbs = on_change_;
    }
    for (auto& cb : cbs) cb(key);
}

// ============================ S7: SMTP admin API =============================

namespace {
// UPSERT one (key, json-value) row with updated_by='admin'.
Status UpsertKey(sqlite3* db, const std::string& key, const std::string& json) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO auth_config(key, value, updated_at, updated_by) "
        "VALUES(?, ?, ?, 'admin') "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value, "
        "updated_at=excluded.updated_at, updated_by='admin'";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "smtp upsert prepare failed");
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, NowMs());
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "smtp upsert failed");
    }
    return Status::Ok();
}
}  // namespace

Status AuthConfigService::SetSmtp(const SmtpSettings& s) {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "platform.db handle is null");
    }
    // JSON-encode each value (string values via nlohmann to escape correctly).
    const std::vector<std::pair<std::string, std::string>> rows = {
        {keys::kSmtpHost, nlohmann::json(s.host).dump()},
        {keys::kSmtpPort, std::to_string(s.port)},
        {keys::kSmtpUser, nlohmann::json(s.user).dump()},
        {keys::kSmtpPass, nlohmann::json(s.pass).dump()},
        {keys::kSmtpTls, s.tls ? "true" : "false"},
        {keys::kEmailVerification, s.enable_email_verification ? "true" : "false"},
    };

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "smtp begin failed");
    }
    for (const auto& [k, v] : rows) {
        Status st = UpsertKey(db_, k, v);
        if (!st.ok()) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return st;
        }
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "smtp commit failed");
    }

    // Hot-reload each written key into the snapshot + fire OnChange (topic 8).
    for (const auto& [k, v] : rows) {
        Status st = ReloadKey(k);
        if (!st.ok()) return st;
    }
    return Status::Ok();
}

AuthConfigService::SmtpSettings AuthConfigService::GetSmtpRedacted() const {
    config::AuthConfig c = Get();
    SmtpSettings s;
    s.host = c.smtp_host;
    s.port = c.smtp_port;
    s.user = c.smtp_user;
    s.pass = c.smtp_pass.empty() ? "" : "***";  // never return the stored password
    s.tls = c.smtp_tls;
    s.enable_email_verification = c.email_verification;
    return s;
}

}  // namespace cortrix::auth
