#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/config/auth_config.h"

// Forward-declare the SQLite handle; full <sqlite3.h> is pulled in by the .cpp.
typedef struct sqlite3 sqlite3;

namespace cortrix::auth {

/// Loads + holds the runtime AuthConfig over platform.db `auth_config`
/// (minimal IGlobalConfig). This is the auth-domain owner of
/// the table; the canonical cortrix::IGlobalConfig stays the repo-wide interface
/// and S7 bridges this to it (PlatformDbAuthConfig) when the admin API lands.
///
/// S1 scope (this header):
///   - LoadOrInitDefaults(): on first start (empty table) write the §3.6 default
///     key set, then load every row into the in-memory `config_` snapshot.
///   - thread-safe Get() of the snapshot; OnChange() registration.
/// S7 will add the admin-API setters (SetSmtp / rotate trigger) that mutate rows
/// and fire OnChange — wiring those to an HTTP route is D3.5.
class AuthConfigService {
public:
    /// `db` is an open platform.db handle whose `auth_config` table already
    /// exists (P08AuthSchemaProvider migrated it). The service does not own `db`.
    explicit AuthConfigService(sqlite3* db) : db_(db) {}

    /// Idempotent startup init:
    ///   - if `auth_config` is empty → INSERT the §3.6 defaults (updated_by='system');
    ///   - then SELECT all rows → populate `config_`.
    /// Re-running on a populated table only reloads (no duplicate writes).
    Status LoadOrInitDefaults();

    /// Thread-safe copy of the current in-memory config snapshot.
    config::AuthConfig Get() const;

    /// Register a callback invoked with the changed key whenever a value is
    /// mutated through this service (S7 admin-API setters fire it for SMTP hot
    /// reload, topic 8). Mirrors IGlobalConfig::OnChange.
    void OnChange(std::function<void(const std::string&)> cb);

    /// Re-read a single `key` from platform.db into the in-memory snapshot and
    /// fire OnChange(key). This is the hot-reload primitive (topic 8): S7's admin
    /// API writes the row, then calls ReloadKey so the live SmtpEmailSender picks
    /// it up without a restart. Returns NotFound if the key is absent.
    Status ReloadKey(const std::string& key);

    // ------------------------------- S7 ---------------------------------

    /// SMTP settings for the admin API (`POST /admin/config/smtp`).
    struct SmtpSettings {
        std::string host;
        int port = 587;
        std::string user;
        std::string pass;
        bool tls = true;
        bool enable_email_verification = false;
    };

    /// Persist SMTP credentials + the email_verification flag
    /// to platform.db `auth_config`, then hot-reload the affected keys
    /// (fires OnChange so a live SmtpEmailSender re-reads). One tx for all keys.
    Status SetSmtp(const SmtpSettings& s);

    /// Current SMTP settings for the GET endpoint, with `pass` REDACTED to "***"
    /// when set (never return the stored password). Reads the snapshot.
    SmtpSettings GetSmtpRedacted() const;

private:
    /// Apply one (key, json-value) row onto `config_`. Unknown keys are ignored
    /// (forward-compatible: a newer db may carry keys this build doesn't model).
    void ApplyRow(const std::string& key, const std::string& json_value);

    /// Invoke all registered OnChange callbacks for `key` (used by S7 setters).
    void NotifyChange(const std::string& key);

    sqlite3* db_;
    mutable std::mutex mu_;
    config::AuthConfig config_;
    std::vector<std::function<void(const std::string&)>> on_change_;
};

}  // namespace cortrix::auth
