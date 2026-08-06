#include "cortrix/query/authorize_namespaces.h"

#include <unordered_set>

#include "cortrix/query/cross_ns_error.h"

namespace cortrix::query {

namespace {

// De-duplicate while preserving first-seen order (the response echoes
// namespaces_queried in request order).
std::vector<std::string> DedupPreserveOrder(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    out.reserve(in.size());
    for (const auto& s : in) {
        if (seen.insert(s).second) out.push_back(s);
    }
    return out;
}

bool IsWildcard(const std::vector<std::string>& requested) {
    return requested.size() == 1 && requested[0] == "*";
}

// The frozen AuthContext (auth/auth_context.h) carries no `role` field;'s
// `auth.role` is resolved by the real tenant service once wired. Standalone we pass "" — the
// MockPermissionService keys off user_id/tenant_id, so role is not load-bearing yet.
std::string RoleOf(const AuthContext&) { return std::string(); }

}  // namespace

std::vector<std::string> ExpandNamespaces(const std::vector<std::string>& requested,
                                          const AuthContext& auth,
                                          PermissionService* perm_service,
                                          int max_namespaces) {
    std::vector<std::string> expanded;
    if (IsWildcard(requested)) {
        // ["*"] → the principal's authorized set (topic 1.5). The cap below still
        // applies, so a principal authorized for > cap NS gets CX_ERR_TOO_MANY.
        expanded = DedupPreserveOrder(
            perm_service->ListAuthorizedNamespaces(auth.user_id, auth.tenant_id, RoleOf(auth)));
    } else {
        expanded = DedupPreserveOrder(requested);
    }

    const int cap = max_namespaces < 1 ? 1 : max_namespaces;
    if (static_cast<int>(expanded.size()) > cap) {
        nlohmann::json sd = nlohmann::json::object();
        sd["requested_count"] = static_cast<int>(expanded.size());
        sd["max_namespaces"] = cap;
        throw CrossNsException(
            CrossNsErrorCode::kTooManyNamespaces, std::move(sd),
            "Requested namespace count exceeds the per-query limit");
    }
    return expanded;
}

std::vector<std::string> AuthorizeNamespaces(const std::vector<std::string>& requested,
                                             const AuthContext& auth,
                                             PermissionService* perm_service,
                                             int max_namespaces) {
    // Step 1: authentication — empty user_id == unauthenticated (v1.0.3).
    if (auth.user_id.empty()) {
        throw CrossNsException(CrossNsErrorCode::kAuthInvalidCredentials, {},
                               "Request is not authenticated");
    }

    // Step 2: expand ["*"] + enforce the hard cap (throws kTooManyNamespaces).
    std::vector<std::string> expanded =
        ExpandNamespaces(requested, auth, perm_service, max_namespaces);

    // Step 3: dynamic batch authorization against ns_acl (tenant service pending; mock now).
    BatchCheckResult check = perm_service->BatchCheck(
        auth.user_id, auth.tenant_id, RoleOf(auth), expanded, PermissionAction::kQuery);

    // Step 4: any unauthorized → reject the whole request (no partial query of a
    // request that contains an unauthorized NS — "reject the whole request on
    // missing permission"). anti-enumeration: not-found NS arrive here as unauthorized.
    if (!check.unauthorized.empty()) {
        throw CrossNsException(
            CrossNsErrorCode::kNsUnauthorized,
            nlohmann::json{{"unauthorized_namespaces", check.unauthorized}},
            "Some namespaces are unauthorized");
    }

    // Step 5: all authorized.
    return expanded;
}

}  // namespace cortrix::query
