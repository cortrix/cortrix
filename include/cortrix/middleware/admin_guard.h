#pragma once
// F20 Security Hardening — AdminGuard middleware (design topic 3, plan A).
//
// Cortrix listens on a single socket (0.0.0.0:port); admin endpoints are not on
// a separate bind. Instead AdminGuard runs as an httplib pre-routing handler:
// for any request whose path matches an admin prefix it checks the client IP
// against the admin whitelist and returns 403 CX_ERR_ADMIN_LOOPBACK_REQUIRED
// unless the client is allowed (loopback by default). This matches how
// Prometheus / Grafana / Etcd protect admin surfaces, and keeps F24's
// `ports: <p>:<p>` single mapping unchanged (design sec 5.1 / 5.2.bis).
//
// V3-E-06 hard-fail: ValidateConfigOrAbort() validates the env combination at
// startup. CORTRIX_ADMIN_BIND=0.0.0.0 without CORTRIX_ALLOW_PUBLIC_ADMIN=true
// aborts the process (admin whitelisted to the public internet must be explicit).

#include <string>

#include <nlohmann/json.hpp>

#include "httplib.h"

namespace cortrix::middleware {

/// Build the 403 CX_ERR_ADMIN_LOOPBACK_REQUIRED body for a denied client. Shared
/// by the pre-routing handler and exposed for testing the GEN-Agent 4-field
/// error schema. `client_ip` is echoed into structured_data.
nlohmann::json BuildAdminDeniedBody(const std::string& client_ip);

/// Resolved admin-access policy. In Phase 1 the whitelist is a two-value switch
/// (design sec 5.1, G3 M4): "127.0.0.1" = loopback-only (default), "0.0.0.0" =
/// all client IPs (requires explicit acknowledgment). CIDR / multi-IP support
/// is deferred to Phase 1.5 (PHASE2_BACKLOG TD-F20-IP-ALLOWLIST).
struct AdminGuardConfig {
    std::string admin_bind_whitelist = "127.0.0.1";  ///< CORTRIX_ADMIN_BIND
    bool allow_public = false;                        ///< CORTRIX_ALLOW_PUBLIC_ADMIN

    /// Build from CORTRIX_ADMIN_BIND / CORTRIX_ALLOW_PUBLIC_ADMIN env vars.
    static AdminGuardConfig FromEnv();
};

/// True if the path is an admin endpoint that AdminGuard protects. Exact-prefix
/// match against the hardcoded admin prefixes (design sec 5.2.bis): not a regex,
/// not configurable. Business / health / metrics paths return false (pass through).
bool IsAdminPath(const std::string& path);

/// True if the address is an IPv4/IPv6 loopback (127.0.0.0/8, ::1, and the
/// ::ffff:127.x.x.x v4-mapped form). Empty / non-loopback -> false.
bool IsLoopback(const std::string& client_addr);

/// AdminGuard policy object: decides whether a given client IP may reach admin
/// paths under the resolved config.
class AdminGuard {
public:
    explicit AdminGuard(AdminGuardConfig config) : config_(std::move(config)) {}

    /// True if `client_addr` is permitted to call admin endpoints:
    ///   - whitelist "0.0.0.0" -> always (public admin acknowledged at startup)
    ///   - otherwise -> loopback clients only
    bool IsClientAllowed(const std::string& client_addr) const;

    const AdminGuardConfig& config() const { return config_; }

    /// V3-E-06 hard-fail: validate the env combination. If admin is whitelisted
    /// to 0.0.0.0 without CORTRIX_ALLOW_PUBLIC_ADMIN=true, print the RED warning
    /// banner and std::exit(1) (service startup aborts). Acknowledged public
    /// admin prints a warning banner and continues. Call once, before installing
    /// the guard.
    static void ValidateConfigOrAbort();

private:
    AdminGuardConfig config_;
};

/// Install AdminGuard as a pre-routing handler on `server`. Runs before route
/// dispatch (and before API-key auth) so admin paths are IP-filtered first.
/// Non-admin paths are untouched. The config is read from env (FromEnv()).
///
/// main.cpp adds a single call to this after route registration (shared-file
/// pure-ADD per D-R1 brief sec 2).
void InstallAdminGuard(httplib::Server& server);

/// Test/DI overload: install with an explicit config instead of reading env.
void InstallAdminGuard(httplib::Server& server, AdminGuardConfig config);

}  // namespace cortrix::middleware
