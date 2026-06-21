#pragma once
#include <string>

namespace cortrix {

/// Permission bitmask
enum Permission : int {
    kPermRead  = 1,  // 0b001
    kPermWrite = 2,  // 0b010
    kPermAdmin = 4,  // 0b100
};

struct AuthContext {
    std::string tenant_id;
    std::string user_id;
    std::string agent_id;
    int permissions = 0;

    bool can_read() const { return permissions & kPermRead; }
    bool can_write() const { return permissions & kPermWrite; }
    bool is_admin() const { return permissions & kPermAdmin; }
    // NOTE: the static per-principal `namespaces` allow-list and
    // can_access_namespace() were removed (ARCHITECTURE V6 / P08 issue 3.3-4).
    // Namespace authorization is now a runtime PermissionService::BatchCheck
    // (ownership + ns_acl), reached through ApiKeyAuth::Authorize's seam.
};

}  // namespace cortrix
