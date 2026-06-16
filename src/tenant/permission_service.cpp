#include <cstdint>
#include "cortrix/tenant/permission_service.h"

#include <chrono>
#include <string>

#include <sqlite3.h>

#include "cortrix/tenant/tenant_error.h"

namespace cortrix::tenant {

namespace {

int64_t NowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool PermissionService::NsExists(const NsId& ns_id) {
    if (db_ == nullptr) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM namespaces WHERE ns_id=? LIMIT 1", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

std::string PermissionService::Visibility(const NsId& ns_id) {
    if (db_ == nullptr) return "";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT visibility FROM namespaces WHERE ns_id=? LIMIT 1", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return "";
    }
    sqlite3_bind_text(stmt, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
    std::string vis;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* v = sqlite3_column_text(stmt, 0);
        if (v) vis = reinterpret_cast<const char*>(v);
    }
    sqlite3_finalize(stmt);
    return vis;
}

// ---------------------------------------------------------------------------
// Structural layer
// ---------------------------------------------------------------------------

bool PermissionService::CheckOwnerTenant(const NsId& ns_id, const TenantId& tenant_id) {
    if (db_ == nullptr) return false;
    // F12 `namespaces.tenant_id` is the owner-tenant FK (P09 sec 6 "owner FK").
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT tenant_id FROM namespaces WHERE ns_id=? LIMIT 1", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
    bool match = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* owner = sqlite3_column_text(stmt, 0);
        if (owner) match = (tenant_id == reinterpret_cast<const char*>(owner));
    }
    sqlite3_finalize(stmt);
    return match;
}

std::optional<NsAclRole> PermissionService::GetAclRole(const NsId& ns_id,
                                                       const TenantId& tenant_id,
                                                       const UserId& user_id) {
    if (db_ == nullptr) return std::nullopt;

    // 1) user-level grant wins (only when a user_id is supplied).
    if (!user_id.empty()) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT role FROM ns_acl WHERE ns_id=? AND grantee_tenant_id=? AND grantee_user_id=? LIMIT 1",
                -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, user_id.c_str(), -1, SQLITE_TRANSIENT);
            std::optional<NsAclRole> role;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* r = sqlite3_column_text(stmt, 0);
                if (r) role = ParseNsAclRole(reinterpret_cast<const char*>(r));
            }
            sqlite3_finalize(stmt);
            if (role) return role;
        }
    }

    // 2) fall back to the tenant-level grant (grantee_user_id IS NULL).
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT role FROM ns_acl WHERE ns_id=? AND grantee_tenant_id=? AND grantee_user_id IS NULL LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<NsAclRole> role;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* r = sqlite3_column_text(stmt, 0);
        if (r) role = ParseNsAclRole(reinterpret_cast<const char*>(r));
    }
    sqlite3_finalize(stmt);
    return role;
}

Status PermissionService::GrantAcl(const NsId& ns_id, const TenantId& grantee_tenant_id,
                                   const UserId& grantee_user_id, NsAclRole role,
                                   const AuthContext& caller_ctx,
                                   const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    if (!NsExists(ns_id)) return TenantStatus(TenantErrorCode::kNsNotFound, ns_id);

    // caller must be the NS owner or an ACL admin grantee.
    if (!CanAdminAcl(caller_ctx, ns_id)) {
        return TenantStatus(TenantErrorCode::kNsUnauthorized,
                            "caller cannot administer ACL on " + ns_id);
    }
    // Cannot grant to the owner tenant (it already has full rights).
    if (CheckOwnerTenant(ns_id, grantee_tenant_id)) {
        return TenantStatus(TenantErrorCode::kAclGrantToOwnerTenant,
                            grantee_tenant_id + " already owns " + ns_id);
    }
    // Duplicate grant: a row at the *exact* requested grain (ns, grantee_tenant,
    // grantee_user-or-NULL) already exists. The ns_acl PK also enforces this, so
    // this is the friendly pre-check; the INSERT path below maps a late constraint
    // hit to the same identity under concurrency.
    {
        sqlite3_stmt* chk = nullptr;
        const char* sql = grantee_user_id.empty()
            ? "SELECT 1 FROM ns_acl WHERE ns_id=? AND grantee_tenant_id=? AND grantee_user_id IS NULL LIMIT 1"
            : "SELECT 1 FROM ns_acl WHERE ns_id=? AND grantee_tenant_id=? AND grantee_user_id=? LIMIT 1";
        if (sqlite3_prepare_v2(db_, sql, -1, &chk, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(chk, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(chk, 2, grantee_tenant_id.c_str(), -1, SQLITE_TRANSIENT);
            if (!grantee_user_id.empty())
                sqlite3_bind_text(chk, 3, grantee_user_id.c_str(), -1, SQLITE_TRANSIENT);
            const bool dup = (sqlite3_step(chk) == SQLITE_ROW);
            sqlite3_finalize(chk);
            if (dup) {
                return TenantStatus(TenantErrorCode::kAclDuplicateGrant,
                                    "ACL already exists on " + ns_id + " for " + grantee_tenant_id);
            }
        }
    }

    const int64_t now = NowSec();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ns_acl(ns_id, grantee_tenant_id, grantee_user_id, role, granted_at, granted_by_user_id) "
            "VALUES(?, ?, ?, ?, ?, ?)",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "grant acl prepare");
    }
    sqlite3_bind_text(stmt, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, grantee_tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    if (grantee_user_id.empty()) {
        sqlite3_bind_null(stmt, 3);
    } else {
        sqlite3_bind_text(stmt, 3, grantee_user_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 4, ToString(role), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_bind_text(stmt, 6, caller_ctx.user_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) {
            return TenantStatus(TenantErrorCode::kAclDuplicateGrant,
                                "ACL already exists on " + ns_id + " for " + grantee_tenant_id);
        }
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("grant acl: ") + sqlite3_errmsg(db_));
    }
    return Status::Ok();
}

Status PermissionService::RevokeAcl(const NsId& ns_id, const TenantId& grantee_tenant_id,
                                    const UserId& grantee_user_id,
                                    const AuthContext& caller_ctx,
                                    const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    if (!NsExists(ns_id)) return TenantStatus(TenantErrorCode::kNsNotFound, ns_id);
    if (!CanAdminAcl(caller_ctx, ns_id)) {
        return TenantStatus(TenantErrorCode::kNsUnauthorized,
                            "caller cannot administer ACL on " + ns_id);
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = grantee_user_id.empty()
        ? "DELETE FROM ns_acl WHERE ns_id=? AND grantee_tenant_id=? AND grantee_user_id IS NULL"
        : "DELETE FROM ns_acl WHERE ns_id=? AND grantee_tenant_id=? AND grantee_user_id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "revoke acl prepare");
    }
    sqlite3_bind_text(stmt, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, grantee_tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    if (!grantee_user_id.empty())
        sqlite3_bind_text(stmt, 3, grantee_user_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("revoke acl: ") + sqlite3_errmsg(db_));
    }
    if (sqlite3_changes(db_) == 0) {
        return TenantStatus(TenantErrorCode::kAclNotFound,
                            "no ACL on " + ns_id + " for " + grantee_tenant_id);
    }
    return Status::Ok();
}

Result<std::vector<AclEntry>> PermissionService::ListAcl(
    const NsId& ns_id, const AuthContext& caller_ctx,
    const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    if (!NsExists(ns_id)) return TenantStatus(TenantErrorCode::kNsNotFound, ns_id);
    if (!CanAdminAcl(caller_ctx, ns_id)) {
        return TenantStatus(TenantErrorCode::kNsUnauthorized,
                            "caller cannot administer ACL on " + ns_id);
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT grantee_tenant_id, grantee_user_id, role, granted_at, granted_by_user_id "
            "FROM ns_acl WHERE ns_id=? ORDER BY granted_at",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "list acl prepare");
    }
    sqlite3_bind_text(stmt, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<AclEntry> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AclEntry e;
        const unsigned char* gt = sqlite3_column_text(stmt, 0);
        const unsigned char* gu = sqlite3_column_text(stmt, 1);
        const unsigned char* r = sqlite3_column_text(stmt, 2);
        const unsigned char* gb = sqlite3_column_text(stmt, 4);
        e.grantee_tenant_id = gt ? reinterpret_cast<const char*>(gt) : "";
        e.grantee_user_id = gu ? reinterpret_cast<const char*>(gu) : "";  // NULL -> ""
        if (r) {
            auto parsed = ParseNsAclRole(reinterpret_cast<const char*>(r));
            if (parsed) e.role = *parsed;
        }
        e.granted_at = sqlite3_column_int64(stmt, 3);
        e.granted_by_user_id = gb ? reinterpret_cast<const char*>(gb) : "";
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

// ---------------------------------------------------------------------------
// Business layer (sec 4.3 composition rules)
// ---------------------------------------------------------------------------

bool PermissionService::CanRead(const AuthContext& ctx, const NsId& ns_id) {
    if (CheckOwnerTenant(ns_id, ctx.tenant_id)) return true;       // owner always reads
    if (GetAclRole(ns_id, ctx.tenant_id, ctx.user_id).has_value()) return true;  // any grant
    if (Visibility(ns_id) == "public") return true;               // public read
    return false;
}

bool PermissionService::CanWrite(const AuthContext& ctx, const NsId& ns_id) {
    if (CheckOwnerTenant(ns_id, ctx.tenant_id)) return true;
    auto role = GetAclRole(ns_id, ctx.tenant_id, ctx.user_id);
    if (role && (*role == NsAclRole::EDITOR || *role == NsAclRole::ADMIN)) return true;
    return false;  // VIEWER cannot write; public is read-only (topic H6)
}

bool PermissionService::CanAdminAcl(const AuthContext& ctx, const NsId& ns_id) {
    if (CheckOwnerTenant(ns_id, ctx.tenant_id)) return true;
    auto role = GetAclRole(ns_id, ctx.tenant_id, ctx.user_id);
    return role && *role == NsAclRole::ADMIN;
}

bool PermissionService::CanDelete(const AuthContext& ctx, const NsId& ns_id) {
    // Owner-only -- an ACL admin grantee cannot (the "last lock", topic H6).
    return CheckOwnerTenant(ns_id, ctx.tenant_id);
}

bool PermissionService::CanTransferOwnership(const AuthContext& ctx, const NsId& ns_id) {
    return CheckOwnerTenant(ns_id, ctx.tenant_id);
}

bool PermissionService::CanChangeVisibility(const AuthContext& ctx, const NsId& ns_id) {
    return CheckOwnerTenant(ns_id, ctx.tenant_id);
}

// ---------------------------------------------------------------------------
// Batch (F04)
// ---------------------------------------------------------------------------

std::vector<PermissionService::BatchPermissionCheck> PermissionService::BatchCheck(
    const AuthContext& ctx, const std::vector<NsId>& ns_ids,
    const observability::TraceContext* /*trace_ctx*/) {
    std::vector<BatchPermissionCheck> out;
    out.reserve(ns_ids.size());
    for (const auto& ns_id : ns_ids) {
        BatchPermissionCheck c;
        c.ns_id = ns_id;
        c.can_read = CanRead(ctx, ns_id);
        c.can_write = CanWrite(ctx, ns_id);
        out.push_back(std::move(c));
    }
    // cortrix_permission_check_total per-action metric wiring is D3.5 deferred.
    return out;
}

}  // namespace cortrix::tenant
