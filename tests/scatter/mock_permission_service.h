#pragma once
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "cortrix/query/permission_service.h"

namespace cortrix::query {

/// MockPermissionService — fixture tenancy for AuthorizeNamespaces tests.
/// The real tenancy (ns_acl table) is **integration**. This fixture is configured with the
/// set of namespaces the principal may QUERY; BatchCheck returns everything else
/// as unauthorized (anti-enumeration: not-found == unauthorized, since "authorized" is the
/// only thing it knows). ListAuthorizedNamespaces backs the ["*"] wildcard.
class MockPermissionService : public PermissionService {
public:
    /// @param authorized  the namespaces the principal can QUERY.
    explicit MockPermissionService(std::vector<std::string> authorized = {})
        : authorized_(authorized.begin(), authorized.end()),
          authorized_order_(std::move(authorized)) {}

    void SetAuthorized(std::vector<std::string> authorized) {
        authorized_ = std::set<std::string>(authorized.begin(), authorized.end());
        authorized_order_ = std::move(authorized);
    }

    BatchCheckResult BatchCheck(const std::string& /*user_id*/,
                                const std::string& /*tenant_id*/,
                                const std::string& /*role*/,
                                const std::vector<std::string>& namespaces,
                                PermissionAction /*action*/) override {
        BatchCheckResult result;
        for (const auto& ns : namespaces) {
            if (authorized_.find(ns) == authorized_.end()) {
                result.unauthorized.push_back(ns);
            }
        }
        return result;
    }

    std::vector<std::string> ListAuthorizedNamespaces(const std::string& /*user_id*/,
                                                      const std::string& /*tenant_id*/,
                                                      const std::string& /*role*/) override {
        return authorized_order_;  // wildcard expands to exactly the authorized set
    }

private:
    std::set<std::string> authorized_;
    std::vector<std::string> authorized_order_;
};

}  // namespace cortrix::query
