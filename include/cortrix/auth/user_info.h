#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cortrix::auth {

/// A tenant membership row as surfaced in API responses (login +
/// /me `tenants` array). Populated from tenant management in later wiring; the struct is
/// owned here because it is part of the auth response shape.
struct TenantRef {
    std::string id;     ///< tenant_id ("tenant-<uuid>")
    std::string name;
    std::string role;   ///< owner | admin | member | viewer
};

/// User profile returned by Register / GetUserInfo / login.
/// `tenants` is filled from tenant management later; empty until then (Register returns the
/// freshly-created user before the Personal-Tenant wiring lands).
struct UserInfo {
    std::string id;             ///< "usr_" + 8 hex
    std::string email;
    std::string display_name;
    bool email_verified = false;
    int64_t created_at = 0;     ///< Unix ts (seconds, per ISO-8601 surface)
    std::vector<TenantRef> tenants;
};

}  // namespace cortrix::auth
