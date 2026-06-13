#pragma once
#include <string>
#include <vector>

#include "cortrix/auth/auth_context.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/observability/trace_context.h"
#include "cortrix/tenant/tenant_types.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::tenant {

/// Tenant lifecycle + membership service over catalog.db (P09 sec 4.2). Owns the
/// CreatePersonal / CreateOrganization flows, member management, and tenant
/// metadata. Built up Story-by-Story:
///   - S2 (this): all 10 methods over the F12 `tenants` / `users` / `user_tenants`
///     tables.
///
/// The service borrows an open catalog.db handle (it does NOT own it). Errors are
/// carried as cortrix::Status via tenant::TenantStatus() -- the API/SDK boundary
/// re-inflates the full Agent-friendly body from the embedded CX_ERR_* token (the
/// sec 4.1 `Result<T,TenantError>` spelling is the pre-convention draft; the project
/// convention is Result<T> + Status, see tenant_error.h).
///
/// Cross-service transaction contract (P09 sec 5.1 / topic M2): CreatePersonal takes
/// a `sqlite3* conn` so P08's AuthService::Register can pass its already-open
/// outermost transaction handle and P09 runs its INSERTs on the SAME connection
/// without opening a nested transaction. P08 keeps BEGIN/COMMIT/ROLLBACK; a P09
/// failure rolls back users + tenants + user_tenants atomically. (The sec 4.2 draft
/// names this `IDbConnection*`; the catalog DB layer is a raw sqlite3* throughout
/// this codebase -- see auth::AuthService -- so we pass the handle directly.)
///
/// V1.0 OSS scope: CreateOrganization runs against UnlimitedPlanProvider-backed
/// quota via the injected QuotaService is NOT wired here (standalone); quota
/// enforcement for org creation is a D3.5 cross-feature concern. CreateOrganization
/// performs the structural INSERTs + caller-authz only in this Wave.
class TenantService {
public:
    explicit TenantService(sqlite3* catalog_db) : db_(catalog_db) {}

    // --- Create (topic 6 -- two interfaces) ---

    /// Personal tenant -- called inside P08's registration flow (no AuthContext,
    /// the user is not yet logged in). Atomic on `conn`: INSERT tenants(type=
    /// 'personal', name=email, dedup_scope='tenant') + INSERT user_tenants(role=
    /// 'owner'). tenant_id = "tenant-" + uuid_v4() (P08 topic 3.1 B). Does NOT
    /// COMMIT -- the caller (P08) owns the transaction. `conn` must be a valid open
    /// handle; if null, the service's own handle is used (standalone path). Email
    /// is assumed already format-validated by P08 (sec 5.1 -- P09 does not re-validate).
    /// Errors: CX_ERR_TENANT_EMAIL_DUPLICATE / CX_ERR_TENANT_TRANSACTION_FAILED.
    Result<Tenant> CreatePersonal(
        const UserId& user_id,
        const std::string& email,
        sqlite3* conn,
        const observability::TraceContext* ctx = nullptr);

    /// Organization tenant -- Cloud V1+ admin / self-serve. INSERT tenants(type=
    /// 'organization') + INSERT user_tenants(creator, role='owner'). Errors:
    /// CX_ERR_TENANT_NAME_DUPLICATE / CX_ERR_TENANT_ALREADY_EXISTS /
    /// CX_ERR_TENANT_QUOTA_EXCEEDED (Cloud V1) / CX_ERR_TENANT_TRANSACTION_FAILED.
    Result<Tenant> CreateOrganization(
        const std::string& name,
        const AuthContext& owner_ctx,
        const observability::TraceContext* ctx = nullptr);

    // --- Member management (caller must be OWNER or ADMIN of the tenant) ---

    /// Errors: CX_ERR_CALLER_ROLE_INSUFFICIENT / CX_ERR_MEMBER_ALREADY_EXISTS /
    /// CX_ERR_MEMBER_ROLE_INVALID / CX_ERR_TENANT_NOT_FOUND.
    Status AddMember(
        const TenantId& tenant_id,
        const UserId& user_id,
        TenantRole role,
        const AuthContext& caller_ctx,
        const observability::TraceContext* ctx = nullptr);

    /// Errors: CX_ERR_CALLER_ROLE_INSUFFICIENT / CX_ERR_MEMBER_NOT_FOUND /
    /// CX_ERR_TENANT_LAST_OWNER_REMOVAL.
    Status RemoveMember(
        const TenantId& tenant_id,
        const UserId& user_id,
        const AuthContext& caller_ctx,
        const observability::TraceContext* ctx = nullptr);

    /// Internal SQL JOIN (sec 4.2 / M4): SELECT u.user_id, u.email, ut.role,
    /// ut.created_at AS joined_at FROM user_tenants ut JOIN users u USING(user_id)
    /// WHERE ut.tenant_id=:tenant_id ORDER BY ut.created_at. Caller must be a member.
    Result<std::vector<Member>> ListMembers(
        const TenantId& tenant_id,
        const AuthContext& caller_ctx,
        const observability::TraceContext* ctx = nullptr);

    /// Errors: CX_ERR_CALLER_ROLE_INSUFFICIENT / CX_ERR_MEMBER_NOT_FOUND /
    /// CX_ERR_MEMBER_ROLE_INVALID / CX_ERR_TENANT_LAST_OWNER_REMOVAL (demoting the
    /// sole owner).
    Status UpdateMemberRole(
        const TenantId& tenant_id,
        const UserId& user_id,
        TenantRole new_role,
        const AuthContext& caller_ctx,
        const observability::TraceContext* ctx = nullptr);

    // --- Tenant metadata ---

    /// Caller must be a member. Errors: CX_ERR_TENANT_NOT_FOUND / CX_ERR_CALLER_NOT_MEMBER.
    Result<Tenant> Get(
        const TenantId& tenant_id,
        const AuthContext& caller_ctx,
        const observability::TraceContext* ctx = nullptr);

    /// Caller must be OWNER/ADMIN. Errors: CX_ERR_TENANT_NOT_FOUND / CX_ERR_CALLER_ROLE_INSUFFICIENT.
    Status UpdateName(
        const TenantId& tenant_id,
        const std::string& new_name,
        const AuthContext& caller_ctx,
        const observability::TraceContext* ctx = nullptr);

    /// Only the OWNER may delete (structural last lock). Refuses if the tenant
    /// still owns namespaces or still has non-owner members. Errors:
    /// CX_ERR_TENANT_DELETE_HAS_NAMESPACES / CX_ERR_TENANT_DELETE_HAS_MEMBERS /
    /// CX_ERR_CALLER_ROLE_INSUFFICIENT / CX_ERR_TENANT_NOT_FOUND.
    Status Delete(
        const TenantId& tenant_id,
        const AuthContext& caller_ctx,
        const observability::TraceContext* ctx = nullptr);

    /// Transfer ownership (only the current OWNER). The target must already be a
    /// member. Errors: CX_ERR_TENANT_TRANSFER_TARGET_NOT_MEMBER /
    /// CX_ERR_CALLER_ROLE_INSUFFICIENT / CX_ERR_TENANT_NOT_FOUND.
    Status TransferOwnership(
        const TenantId& tenant_id,
        const UserId& new_owner_user_id,
        const AuthContext& caller_ctx,
        const observability::TraceContext* ctx = nullptr);

private:
    /// Resolve the caller's role in `tenant_id` (user_tenants lookup). nullopt if
    /// the caller is not a member. Uses caller_ctx.user_id.
    std::optional<TenantRole> CallerRole(sqlite3* conn, const TenantId& tenant_id,
                                         const UserId& user_id) const;

    /// True iff the tenant row exists.
    bool TenantExists(sqlite3* conn, const TenantId& tenant_id) const;

    /// Count members holding role='owner' in the tenant (last-owner guard).
    int OwnerCount(sqlite3* conn, const TenantId& tenant_id) const;

    sqlite3* db_;  ///< borrowed catalog.db handle (not owned)
};

}  // namespace cortrix::tenant
