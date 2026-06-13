#include "cortrix/tenant/tenant_types.h"

namespace cortrix::tenant {

const char* ToString(TenantRole role) {
    switch (role) {
        case TenantRole::OWNER:  return "owner";
        case TenantRole::ADMIN:  return "admin";
        case TenantRole::MEMBER: return "member";
        case TenantRole::VIEWER: return "viewer";
    }
    return "member";  // defensive (unreachable for a valid enum)
}

const char* ToString(NsAclRole role) {
    switch (role) {
        case NsAclRole::VIEWER: return "viewer";
        case NsAclRole::EDITOR: return "editor";
        case NsAclRole::ADMIN:  return "admin";
    }
    return "viewer";
}

const char* ToString(TenantType type) {
    switch (type) {
        case TenantType::PERSONAL:     return "personal";
        case TenantType::ORGANIZATION: return "organization";
        case TenantType::SYSTEM:       return "system";
    }
    return "personal";
}

const char* ToString(QuotaType type) {
    switch (type) {
        case QuotaType::MAX_NAMESPACES:        return "max_namespaces";
        case QuotaType::MAX_DOCUMENTS_PER_NS:  return "max_documents_per_ns";
        case QuotaType::MAX_STORAGE_BYTES:     return "max_storage_bytes";
        case QuotaType::MAX_QPS:               return "max_qps";
        case QuotaType::MAX_MEMBERS_PER_TENANT: return "max_members_per_tenant";
    }
    return "max_namespaces";
}

std::optional<TenantRole> ParseTenantRole(const std::string& token) {
    if (token == "owner")  return TenantRole::OWNER;
    if (token == "admin")  return TenantRole::ADMIN;
    if (token == "member") return TenantRole::MEMBER;
    if (token == "viewer") return TenantRole::VIEWER;
    return std::nullopt;
}

std::optional<NsAclRole> ParseNsAclRole(const std::string& token) {
    if (token == "viewer") return NsAclRole::VIEWER;
    if (token == "editor") return NsAclRole::EDITOR;
    if (token == "admin")  return NsAclRole::ADMIN;
    return std::nullopt;
}

std::optional<TenantType> ParseTenantType(const std::string& token) {
    if (token == "personal")     return TenantType::PERSONAL;
    if (token == "organization") return TenantType::ORGANIZATION;
    if (token == "system")       return TenantType::SYSTEM;
    return std::nullopt;
}

}  // namespace cortrix::tenant
