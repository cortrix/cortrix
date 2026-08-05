#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace cortrix::tenant {

// ===== Role enums (two independent sets, never interchangeable) =====

/// In-tenant role -- mirrors the user_tenants.role physical schema (4 values).
/// Lark/Feishu-style membership ladder.
enum class TenantRole { OWNER, ADMIN, MEMBER, VIEWER };

/// Cross-tenant namespace-sharing role -- mirrors the ns_acl.role physical schema
/// (3 values). Disjoint from TenantRole on purpose.
enum class NsAclRole { VIEWER, EDITOR, ADMIN };

// ===== Tenant type =====
enum class TenantType { PERSONAL, ORGANIZATION, SYSTEM };

// ===== Quota type =====
enum class QuotaType {
    MAX_NAMESPACES,
    MAX_DOCUMENTS_PER_NS,
    MAX_STORAGE_BYTES,
    MAX_QPS,
    MAX_MEMBERS_PER_TENANT,
};

/// A single quota ceiling. limit < 0 means "no limit" (the V1.0 OSS default via
/// UnlimitedPlanProvider).
struct QuotaLimit {
    int64_t limit;  // -1 == unlimited
    static QuotaLimit Unlimited() { return {-1}; }
    bool IsUnlimited() const { return limit < 0; }
};

/// The full quota picture for a tenant: per-type usage + per-type limit.
struct QuotaInfo {
    std::string tenant_id;
    std::map<QuotaType, int64_t> usage;
    std::map<QuotaType, QuotaLimit> limits;
};

// ===== Tenant data structures =====

/// A tenant row (catalog.db `tenants`).
struct Tenant {
    std::string tenant_id;    ///< "tenant-" + uuid_v4()
    std::string name;
    TenantType type = TenantType::PERSONAL;
    std::string dedup_scope;  ///< 'none' / 'ns' / 'tenant' (default) / 'cross_tenant' (V2.0)
    int64_t created_at = 0;
};

/// A tenant member view (user_tenants JOIN users, ListMembers).
struct Member {
    std::string user_id;
    std::string email;
    TenantRole role = TenantRole::MEMBER;
    int64_t joined_at = 0;
};

// ===== Type aliases (consumer-friendly) =====
using TenantId = std::string;
using UserId = std::string;
using NsId = std::string;

// ===== Enum <-> physical schema string mapping =====
// The canonical mapping between the enums above and the lowercase string tokens
// stored in catalog.db (user_tenants.role / ns_acl.role / tenants.type). Kept in
// one place so SQL read/write paths never drift.

const char* ToString(TenantRole role);
const char* ToString(NsAclRole role);
const char* ToString(TenantType type);
const char* ToString(QuotaType type);

/// Parse a user_tenants.role token. Returns std::nullopt for an unknown token.
std::optional<TenantRole> ParseTenantRole(const std::string& token);
/// Parse an ns_acl.role token. Returns std::nullopt for an unknown token.
std::optional<NsAclRole> ParseNsAclRole(const std::string& token);
/// Parse a tenants.type token. Returns std::nullopt for an unknown token.
std::optional<TenantType> ParseTenantType(const std::string& token);

}  // namespace cortrix::tenant
