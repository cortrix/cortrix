#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "cortrix/auth/api_key_service.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::auth {

/// First-time admin bootstrap (P08 §2.13, topic 7 D). On first start (the
/// platform.db `users` table is empty) the server prints a one-time, 60-second,
/// single-use URL; visiting it mints a `system admin` user + an admin API Key
/// (shown once), then the token is invalidated. Two endpoints (GET HTML + POST
/// JSON) share ONE token (V3 ruling 5).
///
/// The token lives in memory only (never persisted). This class owns the token
/// lifecycle + the consume logic; the HTTP routes + the stdout banner are D3.5
/// wiring. Borrows an open platform.db handle + an ApiKeyService.
class BootstrapHandler {
public:
    BootstrapHandler(sqlite3* db, ApiKeyService* api_keys)
        : db_(db), api_keys_(api_keys) {}

    /// True if this is a first start (no rows in `users`) → bootstrap is needed.
    Result<bool> IsFirstStart() const;

    /// If first start, generate + store a fresh 60s single-use token and return
    /// it (for the stdout banner). If users already exist, returns "" (no token,
    /// §S6 `Bootstrap_NotFirstStart`). Regenerates (invalidating any prior token)
    /// — used by `cortrix-setup --rotate-bootstrap` (§S6 `Bootstrap_RotateCommand`).
    Result<std::string> GenerateToken();

    /// The result of consuming the bootstrap token (the one-time admin key).
    struct BootstrapResult {
        std::string admin_api_key;   ///< "cortrix_sk_..." — show once
        std::string admin_user_id;
        std::string admin_key_id;
    };

    /// Consume `token` (P08 §2.13.2 — shared by GET + POST): validate it is the
    /// current, unexpired, unused token → create system admin user (Personal
    /// Tenant placeholder, role=owner) + admin API Key (name='bootstrap-admin') →
    /// invalidate the token. Errors: CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID
    /// (expired | used | not_found). Idempotent failure after consume (token gone).
    Result<BootstrapResult> Consume(const std::string& token);

    /// Render the §2.13.2.a one-time HTML page showing `admin_api_key` (GET path).
    static std::string RenderHtml(const std::string& admin_api_key);

    /// Render the §2.13.2.b JSON body for the POST path.
    static std::string RenderJson(const std::string& admin_api_key);

    /// Token validity window (P08 §2.13.1 — 60 seconds).
    static constexpr int64_t kTokenTtlMs = 60 * 1000;

private:
    sqlite3* db_;
    ApiKeyService* api_keys_;

    mutable std::mutex mu_;
    std::string token_;          ///< "" = no active token
    int64_t token_expires_ms_ = 0;
    bool token_used_ = false;
};

}  // namespace cortrix::auth
