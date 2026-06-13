#pragma once
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
    std::vector<std::string> allowed_namespaces;
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
};

}  // namespace cortrix
