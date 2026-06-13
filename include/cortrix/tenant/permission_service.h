#pragma once
#include <optional>
#include <string>
#include <vector>

#include "cortrix/auth/auth_context.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/observability/trace_context.h"
#include "cortrix/tenant/tenant_types.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::tenant {

/// One ns_acl grant row (P09 sec 4.3).
struct AclEntry {
    TenantId grantee_tenant_id;
    UserId grantee_user_id;  ///< empty == tenant-level grant (stored as NULL)
    NsAclRole role = NsAclRole::VIEWER;
    int64_t granted_at = 0;
    UserId granted_by_user_id;
};

/// Namespace permission service over catalog.db (P09 sec 4.3). Combines the
/// structural layer (owner FK + ns_acl grantee lookups) with the business layer
/// (CanRead/CanWrite/... composite semantics) that the HTTP middleware + feature
/// call sites use. Implements F04's BatchCheck contract (P09 sec 5.2).
///
/// Borrows an open catalog.db handle (not owned). Errors are carried as
/// cortrix::Status via tenant::TenantStatus(); structural boolean checks return a
/// plain bool (no error surface) so they can run on the request hot path.
///
/// V1.0 OSS hot path (P09 sec 4.6): in the single default_tenant deployment the owner
/// check matches immediately (owner_tenant_id == caller.tenant_id) so CanRead/
/// CanWrite return true without touching ns_acl. The full ACL/visibility logic is
/// the same code path (topic 3 -- data state, not code branch, controls behavior).
class PermissionService {
public:
    explicit PermissionService(sqlite3* catalog_db) : db_(catalog_db) {}

    // === Structural layer ===

    /// True iff namespaces.owner_tenant_id (F12 column `tenant_id`) == tenant_id.
    bool CheckOwnerTenant(const NsId& ns_id, const TenantId& tenant_id);

    /// ACL grantee role for (ns, tenant[, user]). A user-level grant wins over a
    /// tenant-level grant; falls back to the tenant-level row when no user row
    /// exists. nullopt when neither is present. `user_id` empty queries the
    /// tenant-level grant directly.
    std::optional<NsAclRole> GetAclRole(const NsId& ns_id, const TenantId& tenant_id,
                                        const UserId& user_id);

    /// Grant an ACL row. caller must be the NS owner or an ACL admin grantee.
    /// `grantee_user_id` empty == tenant-level grant. Errors:
    /// CX_ERR_NS_NOT_FOUND / CX_ERR_NS_UNAUTHORIZED (caller not owner/admin) /
    /// CX_ERR_ACL_GRANT_TO_OWNER_TENANT / CX_ERR_ACL_DUPLICATE_GRANT.
    Status GrantAcl(const NsId& ns_id, const TenantId& grantee_tenant_id,
                    const UserId& grantee_user_id, NsAclRole role,
                    const AuthContext& caller_ctx,
                    const observability::TraceContext* ctx = nullptr);

    /// Revoke an ACL row. caller must be the NS owner or an ACL admin grantee.
    /// Errors: CX_ERR_NS_NOT_FOUND / CX_ERR_NS_UNAUTHORIZED / CX_ERR_ACL_NOT_FOUND.
    Status RevokeAcl(const NsId& ns_id, const TenantId& grantee_tenant_id,
                     const UserId& grantee_user_id, const AuthContext& caller_ctx,
                     const observability::TraceContext* ctx = nullptr);

    /// List all ACL rows for a namespace. caller must be the NS owner or an ACL
    /// admin grantee. Errors: CX_ERR_NS_NOT_FOUND / CX_ERR_NS_UNAUTHORIZED.
    Result<std::vector<AclEntry>> ListAcl(const NsId& ns_id, const AuthContext& caller_ctx,
                                          const observability::TraceContext* ctx = nullptr);

    // === Business layer (composite semantics -- sec 4.3 rules) ===

    bool CanRead(const AuthContext& ctx, const NsId& ns_id);
    bool CanWrite(const AuthContext& ctx, const NsId& ns_id);
    bool CanAdminAcl(const AuthContext& ctx, const NsId& ns_id);

    // Owner-only (an ACL admin grantee cannot -- the "last lock").
    bool CanDelete(const AuthContext& ctx, const NsId& ns_id);
    bool CanTransferOwnership(const AuthContext& ctx, const NsId& ns_id);
    bool CanChangeVisibility(const AuthContext& ctx, const NsId& ns_id);

    // === Batch (F04 Cross-NS Query, P09 sec 5.2) ===
    struct BatchPermissionCheck {
        NsId ns_id;
        bool can_read = false;
        bool can_write = false;
    };
    std::vector<BatchPermissionCheck> BatchCheck(
        const AuthContext& ctx, const std::vector<NsId>& ns_ids,
        const observability::TraceContext* trace_ctx = nullptr);

private:
    /// namespaces.visibility token for ns_id ("" if the namespace does not exist).
    std::string Visibility(const NsId& ns_id);
    /// True iff the namespaces row exists.
    bool NsExists(const NsId& ns_id);

    sqlite3* db_;  ///< borrowed catalog.db handle (not owned)
};

}  // namespace cortrix::tenant
