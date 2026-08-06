#pragma once
#include <string>
#include <vector>

namespace cortrix::query {

/// Action being authorized. The cross-NS path only ever requests QUERY;
/// the enum mirrors the tenant service's surface so the real one drops in unchanged.
enum class PermissionAction {
    kQuery,
};

/// Result of a batch permission check. `unauthorized` is the subset of
/// the requested namespaces the principal may NOT perform the action on.
///
/// Anti-enumeration (topic 2.6): the service does NOT distinguish "NS does not exist" from "NS
/// exists but is unauthorized" — a non-existent NS is reported as unauthorized, so
/// the existence of a namespace is never leaked through this boundary.
struct BatchCheckResult {
    std::vector<std::string> unauthorized;
};

/// PermissionService — the minimal consumer-side view of the tenant permission service.
///
/// 🚨 Standalone: the real PermissionService (queries the ns_acl table) is
/// NOT built yet; wiring it in comes later. The cross-NS path defines this minimal contract
/// + ExpandNamespaces' "all namespaces" source so AuthorizeNamespaces is fully
/// unit-testable against MockPermissionService now.
class PermissionService {
public:
    virtual ~PermissionService() = default;

    /// Batch-check `namespaces` for (`user_id`, `tenant_id`, `role`, `action`).
    /// Returns the unauthorized subset. An empty result == all authorized.
    virtual BatchCheckResult BatchCheck(const std::string& user_id,
                                        const std::string& tenant_id,
                                        const std::string& role,
                                        const std::vector<std::string>& namespaces,
                                        PermissionAction action) = 0;

    /// All namespaces the principal can QUERY in `tenant_id` (the universe that
    /// `namespaces:["*"]` expands to). The real service reads ns_acl; the mock
    /// returns a fixture set. Used only on the wildcard path.
    virtual std::vector<std::string> ListAuthorizedNamespaces(
        const std::string& user_id,
        const std::string& tenant_id,
        const std::string& role) = 0;
};

}  // namespace cortrix::query
