#include <cstdint>
#include "cortrix/tenant/tenant_service.h"

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

#include <openssl/rand.h>
#include <sqlite3.h>

#include "cortrix/tenant/tenant_error.h"

namespace cortrix::tenant {

namespace {

int64_t NowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// RFC-4122 v4 UUID (8-4-4-4-12 hex). Uses the OpenSSL CSPRNG already linked into
/// cortrix_core (same primitive auth::GenerateUserId relies on).
std::string UuidV4() {
    unsigned char b[16];
    RAND_bytes(b, sizeof(b));
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);  // variant 10xx
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 16; ++i) {
        s.push_back(kHex[b[i] >> 4]);
        s.push_back(kHex[b[i] & 0x0F]);
        if (i == 3 || i == 5 || i == 7 || i == 9) s.push_back('-');
    }
    return s;
}

/// Whether a tenant role meets the "manager" bar (OWNER or ADMIN) required to
/// mutate membership / metadata.
bool IsManager(TenantRole role) {
    return role == TenantRole::OWNER || role == TenantRole::ADMIN;
}

}  // namespace

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::optional<TenantRole> TenantService::CallerRole(sqlite3* conn,
                                                    const TenantId& tenant_id,
                                                    const UserId& user_id) const {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn,
                           "SELECT role FROM user_tenants WHERE tenant_id=? AND user_id=? LIMIT 1",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<TenantRole> role;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* txt = sqlite3_column_text(stmt, 0);
        if (txt) role = ParseTenantRole(reinterpret_cast<const char*>(txt));
    }
    sqlite3_finalize(stmt);
    return role;
}

bool TenantService::TenantExists(sqlite3* conn, const TenantId& tenant_id) const {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn, "SELECT 1 FROM tenants WHERE tenant_id=? LIMIT 1", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

int TenantService::OwnerCount(sqlite3* conn, const TenantId& tenant_id) const {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn,
                           "SELECT COUNT(*) FROM user_tenants WHERE tenant_id=? AND role='owner'",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------

Result<Tenant> TenantService::CreatePersonal(const UserId& user_id,
                                             const std::string& email,
                                             sqlite3* conn,
                                             const observability::TraceContext* /*ctx*/) {
    // Reuse the caller's transaction connection when provided (registration holds the outermost tx -- this service does
    // NOT open a nested transaction); fall back to the owned handle standalone.
    sqlite3* db = conn ? conn : db_;
    if (db == nullptr) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            "no database connection");
    }

    const std::string tenant_id = "tenant-" + UuidV4();
    const int64_t now = NowSec();

    // INSERT tenants(type='personal', name=email, dedup_scope='tenant').
    sqlite3_stmt* t = nullptr;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO tenants(tenant_id, name, type, dedup_scope, created_at) "
            "VALUES(?, ?, 'personal', 'tenant', ?)",
            -1, &t, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "tenants insert prepare");
    }
    sqlite3_bind_text(t, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(t, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(t, 3, now);
    int rc = sqlite3_step(t);
    sqlite3_finalize(t);
    if (rc != SQLITE_DONE) {
        // A unique-name/email collision surfaces as the email-duplicate identity
        // (tenants.name carries the email for personal tenants).
        if (rc == SQLITE_CONSTRAINT) {
            return TenantStatus(TenantErrorCode::kTenantEmailDuplicate, email);
        }
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("tenants insert: ") + sqlite3_errmsg(db));
    }

    // INSERT user_tenants(role='owner').
    sqlite3_stmt* ut = nullptr;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO user_tenants(user_id, tenant_id, role, created_at) "
            "VALUES(?, ?, 'owner', ?)",
            -1, &ut, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "user_tenants insert prepare");
    }
    sqlite3_bind_text(ut, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ut, 2, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ut, 3, now);
    rc = sqlite3_step(ut);
    sqlite3_finalize(ut);
    if (rc != SQLITE_DONE) {
        // Membership insert failed -> surface the transaction-failed identity so the
        // caller ROLLBACKs the whole registration (users + tenants + user_tenants).
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("user_tenants insert: ") + sqlite3_errmsg(db));
    }

    Tenant out;
    out.tenant_id = tenant_id;
    out.name = email;
    out.type = TenantType::PERSONAL;
    out.dedup_scope = "tenant";
    out.created_at = now;
    return out;
}

Result<Tenant> TenantService::CreateOrganization(const std::string& name,
                                                 const AuthContext& owner_ctx,
                                                 const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no database connection");
    }
    // V1.0 OSS: plan-based org-creation quota is enforced by QuotaService at the
    // Cloud-V1 layer (UnlimitedPlanProvider here). Cross-feature wiring of that
    // check is integration deferred -- this Wave performs the structural INSERTs only.

    const std::string tenant_id = "tenant-" + UuidV4();
    const int64_t now = NowSec();

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "begin tx");
    }

    sqlite3_stmt* t = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO tenants(tenant_id, name, type, dedup_scope, created_at) "
            "VALUES(?, ?, 'organization', 'tenant', ?)",
            -1, &t, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "tenants insert prepare");
    }
    sqlite3_bind_text(t, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(t, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(t, 3, now);
    int rc = sqlite3_step(t);
    sqlite3_finalize(t);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        if (rc == SQLITE_CONSTRAINT) {
            // tenant_id PK collision (extreme uuid clash) vs name duplicate: the PK
            // is the only UNIQUE on tenants, so a constraint hit here is an id clash.
            return TenantStatus(TenantErrorCode::kTenantAlreadyExists, tenant_id);
        }
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("tenants insert: ") + sqlite3_errmsg(db_));
    }

    sqlite3_stmt* ut = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO user_tenants(user_id, tenant_id, role, created_at) "
            "VALUES(?, ?, 'owner', ?)",
            -1, &ut, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "user_tenants insert prepare");
    }
    sqlite3_bind_text(ut, 1, owner_ctx.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ut, 2, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ut, 3, now);
    rc = sqlite3_step(ut);
    sqlite3_finalize(ut);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("user_tenants insert: ") + sqlite3_errmsg(db_));
    }

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "commit tx");
    }

    Tenant out;
    out.tenant_id = tenant_id;
    out.name = name;
    out.type = TenantType::ORGANIZATION;
    out.dedup_scope = "tenant";
    out.created_at = now;
    return out;
}

// ---------------------------------------------------------------------------
// Member management
// ---------------------------------------------------------------------------

Status TenantService::AddMember(const TenantId& tenant_id, const UserId& user_id,
                                TenantRole role, const AuthContext& caller_ctx,
                                const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    if (!TenantExists(db_, tenant_id)) {
        return TenantStatus(TenantErrorCode::kTenantNotFound, tenant_id);
    }
    auto caller = CallerRole(db_, tenant_id, caller_ctx.user_id);
    if (!caller || !IsManager(*caller)) {
        return TenantStatus(TenantErrorCode::kCallerRoleInsufficient,
                            "caller is not OWNER/ADMIN of " + tenant_id);
    }
    // role validity is guaranteed by the TenantRole enum at this layer; the HTTP
    // boundary maps an unknown token to CX_ERR_MEMBER_ROLE_INVALID before here.

    if (CallerRole(db_, tenant_id, user_id).has_value()) {
        return TenantStatus(TenantErrorCode::kMemberAlreadyExists,
                            user_id + " already in " + tenant_id);
    }

    const int64_t now = NowSec();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO user_tenants(user_id, tenant_id, role, created_at) VALUES(?, ?, ?, ?)",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "add member prepare");
    }
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, ToString(role), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, now);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) {
            return TenantStatus(TenantErrorCode::kMemberAlreadyExists, user_id);
        }
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("add member: ") + sqlite3_errmsg(db_));
    }
    return Status::Ok();
}

Status TenantService::RemoveMember(const TenantId& tenant_id, const UserId& user_id,
                                   const AuthContext& caller_ctx,
                                   const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    auto caller = CallerRole(db_, tenant_id, caller_ctx.user_id);
    if (!caller || !IsManager(*caller)) {
        return TenantStatus(TenantErrorCode::kCallerRoleInsufficient,
                            "caller is not OWNER/ADMIN of " + tenant_id);
    }
    auto target = CallerRole(db_, tenant_id, user_id);
    if (!target) {
        return TenantStatus(TenantErrorCode::kMemberNotFound, user_id + " not in " + tenant_id);
    }
    if (*target == TenantRole::OWNER && OwnerCount(db_, tenant_id) <= 1) {
        return TenantStatus(TenantErrorCode::kTenantLastOwnerRemoval,
                            "cannot remove the last owner of " + tenant_id);
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM user_tenants WHERE tenant_id=? AND user_id=?", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "remove member prepare");
    }
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("remove member: ") + sqlite3_errmsg(db_));
    }
    return Status::Ok();
}

Result<std::vector<Member>> TenantService::ListMembers(
    const TenantId& tenant_id, const AuthContext& caller_ctx,
    const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    if (!CallerRole(db_, tenant_id, caller_ctx.user_id).has_value()) {
        return TenantStatus(TenantErrorCode::kCallerNotMember,
                            caller_ctx.user_id + " not in " + tenant_id);
    }

    // sec 4.2 / M4 -- JOIN users to recover email (user_tenants has no email column).
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT u.user_id, u.email, ut.role, ut.created_at "
            "FROM user_tenants ut JOIN users u USING(user_id) "
            "WHERE ut.tenant_id=? ORDER BY ut.created_at",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "list members prepare");
    }
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<Member> members;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Member m;
        const unsigned char* uid = sqlite3_column_text(stmt, 0);
        const unsigned char* email = sqlite3_column_text(stmt, 1);
        const unsigned char* role = sqlite3_column_text(stmt, 2);
        m.user_id = uid ? reinterpret_cast<const char*>(uid) : "";
        m.email = email ? reinterpret_cast<const char*>(email) : "";
        if (role) {
            auto parsed = ParseTenantRole(reinterpret_cast<const char*>(role));
            if (parsed) m.role = *parsed;
        }
        m.joined_at = sqlite3_column_int64(stmt, 3);
        members.push_back(std::move(m));
    }
    sqlite3_finalize(stmt);
    return members;
}

Status TenantService::UpdateMemberRole(const TenantId& tenant_id, const UserId& user_id,
                                       TenantRole new_role, const AuthContext& caller_ctx,
                                       const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    auto caller = CallerRole(db_, tenant_id, caller_ctx.user_id);
    if (!caller || !IsManager(*caller)) {
        return TenantStatus(TenantErrorCode::kCallerRoleInsufficient,
                            "caller is not OWNER/ADMIN of " + tenant_id);
    }
    auto target = CallerRole(db_, tenant_id, user_id);
    if (!target) {
        return TenantStatus(TenantErrorCode::kMemberNotFound, user_id + " not in " + tenant_id);
    }
    // Demoting the sole owner away from OWNER would leave the tenant ownerless.
    if (*target == TenantRole::OWNER && new_role != TenantRole::OWNER &&
        OwnerCount(db_, tenant_id) <= 1) {
        return TenantStatus(TenantErrorCode::kTenantLastOwnerRemoval,
                            "cannot demote the last owner of " + tenant_id);
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE user_tenants SET role=? WHERE tenant_id=? AND user_id=?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "update role prepare");
    }
    sqlite3_bind_text(stmt, 1, ToString(new_role), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, user_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("update role: ") + sqlite3_errmsg(db_));
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Tenant metadata
// ---------------------------------------------------------------------------

Result<Tenant> TenantService::Get(const TenantId& tenant_id, const AuthContext& caller_ctx,
                                  const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    if (!TenantExists(db_, tenant_id)) {
        return TenantStatus(TenantErrorCode::kTenantNotFound, tenant_id);
    }
    // admin API Key (is_admin) may read any tenant; otherwise caller must be a member.
    if (!caller_ctx.is_admin() && !CallerRole(db_, tenant_id, caller_ctx.user_id).has_value()) {
        return TenantStatus(TenantErrorCode::kCallerNotMember,
                            caller_ctx.user_id + " not in " + tenant_id);
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "SELECT tenant_id, name, type, dedup_scope, created_at FROM tenants WHERE tenant_id=?",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "get tenant prepare");
    }
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return TenantStatus(TenantErrorCode::kTenantNotFound, tenant_id);
    }
    Tenant out;
    const unsigned char* tid = sqlite3_column_text(stmt, 0);
    const unsigned char* name = sqlite3_column_text(stmt, 1);
    const unsigned char* type = sqlite3_column_text(stmt, 2);
    const unsigned char* ds = sqlite3_column_text(stmt, 3);
    out.tenant_id = tid ? reinterpret_cast<const char*>(tid) : tenant_id;
    out.name = name ? reinterpret_cast<const char*>(name) : "";
    if (type) {
        auto parsed = ParseTenantType(reinterpret_cast<const char*>(type));
        if (parsed) out.type = *parsed;
    }
    out.dedup_scope = ds ? reinterpret_cast<const char*>(ds) : "tenant";
    out.created_at = sqlite3_column_int64(stmt, 4);
    sqlite3_finalize(stmt);
    return out;
}

Status TenantService::UpdateName(const TenantId& tenant_id, const std::string& new_name,
                                 const AuthContext& caller_ctx,
                                 const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    if (!TenantExists(db_, tenant_id)) {
        return TenantStatus(TenantErrorCode::kTenantNotFound, tenant_id);
    }
    auto caller = CallerRole(db_, tenant_id, caller_ctx.user_id);
    if (!caller || !IsManager(*caller)) {
        return TenantStatus(TenantErrorCode::kCallerRoleInsufficient,
                            "caller is not OWNER/ADMIN of " + tenant_id);
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE tenants SET name=? WHERE tenant_id=?", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "update name prepare");
    }
    sqlite3_bind_text(stmt, 1, new_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("update name: ") + sqlite3_errmsg(db_));
    }
    return Status::Ok();
}

Status TenantService::Delete(const TenantId& tenant_id, const AuthContext& caller_ctx,
                             const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    if (!TenantExists(db_, tenant_id)) {
        return TenantStatus(TenantErrorCode::kTenantNotFound, tenant_id);
    }
    auto caller = CallerRole(db_, tenant_id, caller_ctx.user_id);
    if (!caller || *caller != TenantRole::OWNER) {
        return TenantStatus(TenantErrorCode::kCallerRoleInsufficient,
                            "only the OWNER may delete " + tenant_id);
    }

    // Refuse if the tenant still owns namespaces.
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM namespaces WHERE tenant_id=?", -1, &stmt,
                               nullptr) != SQLITE_OK) {
            return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "ns count prepare");
        }
        sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
        int ns_count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) ns_count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (ns_count > 0) {
            return TenantStatus(TenantErrorCode::kTenantDeleteHasNamespaces,
                                tenant_id + " owns " + std::to_string(ns_count) + " namespace(s)");
        }
    }

    // Refuse if any non-owner member remains.
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                               "SELECT COUNT(*) FROM user_tenants WHERE tenant_id=? AND role<>'owner'",
                               -1, &stmt, nullptr) != SQLITE_OK) {
            return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "member count prepare");
        }
        sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
        int member_count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) member_count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (member_count > 0) {
            return TenantStatus(TenantErrorCode::kTenantDeleteHasMembers,
                                tenant_id + " still has " + std::to_string(member_count) +
                                    " non-owner member(s)");
        }
    }

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "begin tx");
    }
    // Remove the owner membership row(s) then the tenant.
    sqlite3_stmt* d1 = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM user_tenants WHERE tenant_id=?", -1, &d1, nullptr);
    sqlite3_bind_text(d1, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc1 = sqlite3_step(d1);
    sqlite3_finalize(d1);
    sqlite3_stmt* d2 = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM tenants WHERE tenant_id=?", -1, &d2, nullptr);
    sqlite3_bind_text(d2, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc2 = sqlite3_step(d2);
    sqlite3_finalize(d2);
    if (rc1 != SQLITE_DONE || rc2 != SQLITE_DONE) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("delete tenant: ") + sqlite3_errmsg(db_));
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "commit tx");
    }
    return Status::Ok();
}

Status TenantService::TransferOwnership(const TenantId& tenant_id,
                                        const UserId& new_owner_user_id,
                                        const AuthContext& caller_ctx,
                                        const observability::TraceContext* /*ctx*/) {
    if (db_ == nullptr) return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "no db");
    if (!TenantExists(db_, tenant_id)) {
        return TenantStatus(TenantErrorCode::kTenantNotFound, tenant_id);
    }
    auto caller = CallerRole(db_, tenant_id, caller_ctx.user_id);
    if (!caller || *caller != TenantRole::OWNER) {
        return TenantStatus(TenantErrorCode::kCallerRoleInsufficient,
                            "only the current OWNER may transfer ownership of " + tenant_id);
    }
    auto target = CallerRole(db_, tenant_id, new_owner_user_id);
    if (!target) {
        return TenantStatus(TenantErrorCode::kTenantTransferTargetNotMember,
                            new_owner_user_id + " is not a member of " + tenant_id);
    }

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "begin tx");
    }
    // Promote the target to owner, demote the current owner to admin.
    sqlite3_stmt* up = nullptr;
    sqlite3_prepare_v2(db_, "UPDATE user_tenants SET role='owner' WHERE tenant_id=? AND user_id=?",
                       -1, &up, nullptr);
    sqlite3_bind_text(up, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(up, 2, new_owner_user_id.c_str(), -1, SQLITE_TRANSIENT);
    int rcu = sqlite3_step(up);
    sqlite3_finalize(up);
    sqlite3_stmt* dn = nullptr;
    sqlite3_prepare_v2(db_, "UPDATE user_tenants SET role='admin' WHERE tenant_id=? AND user_id=?",
                       -1, &dn, nullptr);
    sqlite3_bind_text(dn, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(dn, 2, caller_ctx.user_id.c_str(), -1, SQLITE_TRANSIENT);
    int rcd = sqlite3_step(dn);
    sqlite3_finalize(dn);
    if (rcu != SQLITE_DONE || rcd != SQLITE_DONE) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed,
                            std::string("transfer ownership: ") + sqlite3_errmsg(db_));
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return TenantStatus(TenantErrorCode::kTenantTransactionFailed, "commit tx");
    }
    return Status::Ok();
}

}  // namespace cortrix::tenant
