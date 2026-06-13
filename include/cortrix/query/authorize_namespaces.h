#pragma once
#include <string>
#include <vector>

#include "cortrix/auth/auth_context.h"          // cortrix::AuthContext
#include "cortrix/query/permission_service.h"

namespace cortrix::query {

/// Default per-query namespace hard cap (topic 1.5 / GUC scatter.max_namespaces_per_query
/// default 100). ScatterGather passes the GUC-resolved value; this constant is the
/// spec default + the value AuthorizeNamespaces/ExpandNamespaces use when none is given.
constexpr int kDefaultMaxNamespaces = 100;

/// ExpandNamespaces — resolve the requested namespace list (topic 1.5 + §4.2 Step 2).
///
/// - `["*"]`  → the principal's full authorized set (perm_service->ListAuthorized…),
///              i.e. NS the Agent may actually QUERY (not the global universe).
/// - otherwise → the list as-is (de-duplicated, order preserved).
///
/// Then enforces the hard cap: if the expanded count exceeds `max_namespaces`,
/// throws CrossNsException(kTooManyNamespaces) with structured_data
/// {requested_count, max_namespaces} (topic 1.5 / GEN-Agent #5 — Agent slices + parallel
/// re-queries). `max_namespaces` is the min(global GUC, plan cap) already taken by
/// the caller (P01-3 plan coordination = S4.3, not done in W2). An empty request list throws
/// CX_ERR_TOO_MANY_NAMESPACES is NOT used — an empty list is an invalid request and
/// is rejected upstream; here an empty (non-wildcard) list simply yields empty.
std::vector<std::string> ExpandNamespaces(
    const std::vector<std::string>& requested,
    const AuthContext& auth,
    PermissionService* perm_service,
    int max_namespaces = kDefaultMaxNamespaces);

/// AuthorizeNamespaces — the §4.2 5-step batch authorization, returns the list of
/// namespaces to actually query (all authorized). Throws CrossNsException on any
/// failure (the handler serializes it to the §2.6 error body):
///
///   Step 1  auth.user_id.empty()                 → CX_ERR_AUTH_INVALID_CREDENTIALS
///   Step 2  ExpandNamespaces (["*"] + hard cap)   → CX_ERR_TOO_MANY_NAMESPACES
///   Step 3  perm_service->BatchCheck(QUERY)       (dynamic ns_acl check)
///   Step 4  any unauthorized                      → CX_ERR_NS_UNAUTHORIZED
///                                                   + structured_data.unauthorized_namespaces
///   Step 5  return expanded (all authorized)
///
/// Anti-enumeration (topic 2.6): a non-existent NS comes back from BatchCheck as unauthorized →
/// CX_ERR_NS_UNAUTHORIZED, never a distinct "not found", so existence isn't leaked.
std::vector<std::string> AuthorizeNamespaces(
    const std::vector<std::string>& requested,
    const AuthContext& auth,
    PermissionService* perm_service,
    int max_namespaces = kDefaultMaxNamespaces);

}  // namespace cortrix::query
