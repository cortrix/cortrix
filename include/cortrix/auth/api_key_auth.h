#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>

#include "cortrix/auth/auth_context.h"
#include "cortrix/common/status.h"

namespace cortrix {

namespace auth { class ApiKeyService; }

struct ApiKeyConfig {
    std::string key_hash;
    std::string tenant_id;
    // NOTE: the per-key static `allowed_namespaces` allow-list was removed
    // (ARCHITECTURE V6 / P08 issue 3.3-4). Namespace authorization is now a
    // runtime PermissionService::BatchCheck (ownership + ns_acl), wired into
    // ApiKeyAuth via SetNamespaceAuthorizer() — least-privilege, immediately
    // reflects Admin grant/revoke without re-issuing the key.
    int permissions = 0;
    int64_t expires_at = 0;
};

class ApiKeyAuth {
public:
    /// Set whether auth is enabled (when false, WithAuth behaves like NoAuth)
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    /// Load API key list from config
    void LoadKeys(const std::vector<ApiKeyConfig>& keys);

    /// [D3.5 r2 · Wave P · P1] Bind the platform.db-backed ApiKeyService so keys
    /// minted at runtime (bootstrap admin key + /auth/api-keys) authenticate too.
    /// When set, Authenticate() falls back to ApiKeyService::ValidateApiKey on a
    /// config-map miss: a valid DB key yields an admin AuthContext (the CE single-
    /// user model — the only keys in platform.db are admin-minted). Borrowed; the
    /// service must outlive this object. nullptr (default) = config keys only.
    void SetApiKeyService(auth::ApiKeyService* svc) { db_keys_ = svc; }

    /// Runtime namespace-authorization seam (ARCHITECTURE V6 / P08 issue 3.3-4).
    /// Returns true iff the principal may access `ns` (the real wiring calls
    /// PermissionService::BatchCheck — ownership + ns_acl). When unset, Authorize
    /// leaves the namespace ungated (e.g. routes with no namespace, or no-auth
    /// dev mode); bootstrap installs the production authorizer. Borrowed state in
    /// the closure must outlive this object.
    using NamespaceAuthorizer =
        std::function<bool(const AuthContext& ctx, const std::string& ns)>;
    void SetNamespaceAuthorizer(NamespaceAuthorizer fn) { ns_authz_ = std::move(fn); }

    /// Runtime "authorized namespace set" seam — the universe a wildcard / list
    /// request resolves to (PermissionService::ListAuthorizedNamespaces). Used by
    /// the GET /namespaces list endpoint so it shows only what the principal may
    /// see. Unset returns nullopt (caller decides the fallback).
    using NamespaceLister =
        std::function<std::vector<std::string>(const AuthContext& ctx)>;
    void SetNamespaceLister(NamespaceLister fn) { ns_lister_ = std::move(fn); }
    bool has_namespace_lister() const { return static_cast<bool>(ns_lister_); }
    std::vector<std::string> ListAuthorizedNamespaces(const AuthContext& ctx) const {
        return ns_lister_ ? ns_lister_(ctx) : std::vector<std::string>{};
    }

    /// Authenticate bearer token, fill AuthContext on success
    /// @return Ok / Unauthenticated
    Status Authenticate(const std::string& bearer_token, AuthContext* context);

    /// Authorize: check if ctx has permission for namespace
    /// @return Ok / PermissionDenied
    Status Authorize(const AuthContext& ctx,
                     const std::string& ns,
                     int required_permission);

    /// Compute SHA-256 hash (static utility)
    /// @return lowercase hex digest (64 chars)
    static std::string HashKey(const std::string& plaintext);

private:
    bool enabled_ = true;
    mutable std::mutex mu_;
    std::unordered_map<std::string, ApiKeyConfig> keys_;
    auth::ApiKeyService* db_keys_ = nullptr;  ///< borrowed; nullptr = config keys only
    NamespaceAuthorizer ns_authz_;  ///< runtime PermissionService seam; unset = ungated
    NamespaceLister ns_lister_;     ///< runtime authorized-set seam (list endpoint)
};

}  // namespace cortrix
