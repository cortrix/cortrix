#pragma once
#include <string>
#include <vector>

namespace cortrix::query {

/// Action being authorized (F04 §4.2 P09::Action). F04 only ever requests QUERY;
/// the enum mirrors P09's surface so the real service drops in unchanged at D3.5.
enum class PermissionAction {
    kQuery,
};

/// Result of a batch permission check (F04 §4.2). `unauthorized` is the subset of
/// the requested namespaces the principal may NOT perform the action on.
///
/// Anti-enumeration (topic 2.6): the service does NOT distinguish "NS does not exist" from "NS
/// exists but is unauthorized" — a non-existent NS is reported as unauthorized, so
/// the existence of a namespace is never leaked through this boundary.
struct BatchCheckResult {
    std::vector<std::string> unauthorized;
};

/// PermissionService — the minimal F04 consumer-side view of P09 (F04 §4.2).
///
/// 🚨 D3 standalone: the real P09 PermissionService (queries the ns_acl table) is
/// NOT built (Wave C); wiring it in is **D3.5**. F04 defines this minimal contract
/// + ExpandNamespaces' "all namespaces" source so AuthorizeNamespaces is fully
/// unit-testable against MockPermissionService now.
class PermissionService {
public:
    virtual ~PermissionService() = default;

    /// Batch-check `namespaces` for (`user_id`, `tenant_id`, `role`, `action`).
    /// Returns the unauthorized subset (§4.2). An empty result == all authorized.
    virtual BatchCheckResult BatchCheck(const std::string& user_id,
                                        const std::string& tenant_id,
                                        const std::string& role,
                                        const std::vector<std::string>& namespaces,
                                        PermissionAction action) = 0;

    /// All namespaces the principal can QUERY in `tenant_id` (the universe that
    /// `namespaces:["*"]` expands to — topic 1.5). Real P09 reads ns_acl; the mock
    /// returns a fixture set. Used only on the wildcard path.
    virtual std::vector<std::string> ListAuthorizedNamespaces(
        const std::string& user_id,
        const std::string& tenant_id,
        const std::string& role) = 0;
};

}  // namespace cortrix::query
