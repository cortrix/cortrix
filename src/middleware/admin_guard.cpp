#include "cortrix/middleware/admin_guard.h"

#include <cstdlib>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/logging/logging.h"

namespace cortrix::middleware {

namespace {

// Admin path prefixes AdminGuard protects (design sec 5.2.bis, exact prefix).
//   /api/v1/admin/*          -> auth / tenant / import / operations admin namespace
//   /api/v1/system/tenants/* -> high-cardinality per-tenant stats (OBS_SPEC)
//   /api/v1/system/units/*   -> high-cardinality per-unit stats (OBS_SPEC)
constexpr const char* kAdminPrefixes[] = {
    "/api/v1/admin/",
    "/api/v1/system/tenants/",
    "/api/v1/system/units/",
};

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

AdminGuardConfig AdminGuardConfig::FromEnv() {
    AdminGuardConfig cfg;
    const char* bind = std::getenv("CORTRIX_ADMIN_BIND");
    if (bind != nullptr && *bind != '\0') {
        cfg.admin_bind_whitelist = bind;
    }
    const char* allow = std::getenv("CORTRIX_ALLOW_PUBLIC_ADMIN");
    if (allow != nullptr) {
        std::string v(allow);
        cfg.allow_public = (v == "true" || v == "1");
    }
    return cfg;
}

bool IsAdminPath(const std::string& path) {
    for (const char* prefix : kAdminPrefixes) {
        if (StartsWith(path, prefix)) return true;
    }
    return false;
}

bool IsLoopback(const std::string& client_addr) {
    if (client_addr.empty()) return false;
    // IPv6 loopback and the IPv4-mapped loopback form.
    if (client_addr == "::1") return true;
    if (StartsWith(client_addr, "::ffff:127.")) return true;
    // IPv4 loopback block 127.0.0.0/8.
    if (StartsWith(client_addr, "127.")) return true;
    return false;
}

bool AdminGuard::IsClientAllowed(const std::string& client_addr) const {
    // Whitelist 0.0.0.0 means "all client IPs" — only reachable once
    // ValidateConfigOrAbort() has confirmed CORTRIX_ALLOW_PUBLIC_ADMIN=true.
    if (config_.admin_bind_whitelist == "0.0.0.0") {
        return true;
    }
    // Default / "127.0.0.1": loopback clients only.
    return IsLoopback(client_addr);
}

nlohmann::json BuildAdminDeniedBody(const std::string& client_ip) {
    agent_friendly::AgentFriendlyError err;
    err.code = "CX_ERR_ADMIN_LOOPBACK_REQUIRED";
    err.message = "Admin endpoints are restricted to loopback clients";
    err.retryable = false;
    err.category = agent_friendly::ErrorCategory::kAuth;
    err.structured_data = nlohmann::json{
        {"client_ip", client_ip},
        {"required", "loopback"},
    };
    // R2-M8: wrapped envelope ("error" key) so SDK/MCP/WebUI parse
    // body["error"]["code"] consistently (this pre-routing 403 was a flat holdout).
    nlohmann::json body;
    body["error"] = agent_friendly::ToJson(err);
    return body;
}

void AdminGuard::ValidateConfigOrAbort() {
    AdminGuardConfig cfg = AdminGuardConfig::FromEnv();

    if (cfg.admin_bind_whitelist != "0.0.0.0") {
        // Default loopback-only or explicit 127.0.0.1 — safe, nothing to warn.
        return;
    }

    if (!cfg.allow_public) {
        // V3-E-06 hard-fail: public admin whitelist not explicitly acknowledged.
        std::fprintf(stderr, "\xF0\x9F\x9A\xA8 RED WARNING \xF0\x9F\x9A\xA8\n");
        std::fprintf(stderr,
            "CORTRIX_ADMIN_BIND=%s will whitelist admin API to ALL client IPs "
            "(public network)!\n", cfg.admin_bind_whitelist.c_str());
        std::fprintf(stderr,
            "This is HIGHLY DANGEROUS in untrusted environments.\n");
        std::fprintf(stderr,
            "To proceed, set CORTRIX_ALLOW_PUBLIC_ADMIN=true to acknowledge the "
            "risk.\n");
        std::fprintf(stderr,
            "(If you intended loopback-only, unset CORTRIX_ADMIN_BIND or set to "
            "127.0.0.1)\n");
        std::fprintf(stderr,
            "Admin middleware initialization ABORTED -> service startup "
            "ABORTED.\n");
        std::exit(1);
    }

    // Acknowledged: continue, but make the dangerous posture loud.
    std::fprintf(stderr,
        "\xE2\x9A\xA0\xEF\xB8\x8F  CORTRIX_ADMIN_BIND=%s admin path whitelist = "
        "ALL client IPs (acknowledged via CORTRIX_ALLOW_PUBLIC_ADMIN=true).\n",
        cfg.admin_bind_whitelist.c_str());
}

namespace {

void InstallGuard(httplib::Server& server, AdminGuard guard) {
    server.set_pre_routing_handler(
        [guard](const httplib::Request& req, httplib::Response& res) {
            if (!IsAdminPath(req.path)) {
                return httplib::Server::HandlerResponse::Unhandled;  // pass through
            }
            if (guard.IsClientAllowed(req.remote_addr)) {
                return httplib::Server::HandlerResponse::Unhandled;  // allowed -> route
            }
            // Denied: 403 with the GEN-Agent 4-field error body.
            res.status = 403;
            res.set_content(BuildAdminDeniedBody(req.remote_addr).dump(),
                            "application/json");
            return httplib::Server::HandlerResponse::Handled;  // short-circuit
        });
}

}  // namespace

void InstallAdminGuard(httplib::Server& server) {
    InstallAdminGuard(server, AdminGuardConfig::FromEnv());
}

void InstallAdminGuard(httplib::Server& server, AdminGuardConfig config) {
    CORTRIX_LOG_INFO("security",
                     "AdminGuard installed (whitelist={}, public={})",
                     config.admin_bind_whitelist, config.allow_public);
    InstallGuard(server, AdminGuard(std::move(config)));
}

}  // namespace cortrix::middleware
