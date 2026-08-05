#include "cortrix/tenant/tenant_error.h"

#include <utility>

namespace cortrix::tenant {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so
// each returns a stable reference. The switch is intentionally exhaustive:
// building with -Wall -Wextra (-Wswitch) turns "added a code without a row" into a
// warning the project treats as a build failure -- the registry can't silently
// drift from the enum.
constexpr TenantErrorInfo kTenantNotFound                {"CX_ERR_TENANT_NOT_FOUND",                 ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kTenantAlreadyExists           {"CX_ERR_TENANT_ALREADY_EXISTS",            ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kTenantEmailDuplicate          {"CX_ERR_TENANT_EMAIL_DUPLICATE",           ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kTenantNameDuplicate           {"CX_ERR_TENANT_NAME_DUPLICATE",            ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kTenantDeleteHasNamespaces     {"CX_ERR_TENANT_DELETE_HAS_NAMESPACES",     ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kTenantDeleteHasMembers        {"CX_ERR_TENANT_DELETE_HAS_MEMBERS",        ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kTenantTransferTargetNotMember {"CX_ERR_TENANT_TRANSFER_TARGET_NOT_MEMBER", ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kTenantLastOwnerRemoval        {"CX_ERR_TENANT_LAST_OWNER_REMOVAL",        ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kMemberAlreadyExists           {"CX_ERR_MEMBER_ALREADY_EXISTS",            ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kMemberNotFound                {"CX_ERR_MEMBER_NOT_FOUND",                 ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kMemberRoleInvalid             {"CX_ERR_MEMBER_ROLE_INVALID",              ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kNsUnauthorized                {"CX_ERR_NS_UNAUTHORIZED",                  ErrorCategory::kAuth,      false, std::nullopt};
constexpr TenantErrorInfo kNsNotFound                    {"CX_ERR_NS_NOT_FOUND",                     ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kAclGrantToOwnerTenant         {"CX_ERR_ACL_GRANT_TO_OWNER_TENANT",        ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kAclDuplicateGrant             {"CX_ERR_ACL_DUPLICATE_GRANT",              ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kAclGrantInvalidRole           {"CX_ERR_ACL_GRANT_INVALID_ROLE",           ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kAclNotFound                   {"CX_ERR_ACL_NOT_FOUND",                    ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kPermissionOwnerOnly           {"CX_ERR_PERMISSION_OWNER_ONLY",            ErrorCategory::kAuth,      false, std::nullopt};
constexpr TenantErrorInfo kTenantQuotaExceeded           {"CX_ERR_TENANT_QUOTA_EXCEEDED",            ErrorCategory::kQuota,     false, std::nullopt};
constexpr TenantErrorInfo kTenantQuotaNotFound           {"CX_ERR_TENANT_QUOTA_NOT_FOUND",           ErrorCategory::kPermanent, false, std::nullopt};
constexpr TenantErrorInfo kTenantTransactionFailed       {"CX_ERR_TENANT_TRANSACTION_FAILED",        ErrorCategory::kTransient, true,  100};
constexpr TenantErrorInfo kAdminKeyRequired              {"CX_ERR_ADMIN_KEY_REQUIRED",               ErrorCategory::kAuth,      false, std::nullopt};
constexpr TenantErrorInfo kCallerNotMember               {"CX_ERR_CALLER_NOT_MEMBER",                ErrorCategory::kAuth,      false, std::nullopt};
constexpr TenantErrorInfo kCallerRoleInsufficient        {"CX_ERR_CALLER_ROLE_INSUFFICIENT",         ErrorCategory::kAuth,      false, std::nullopt};

}  // namespace

const TenantErrorInfo& GetTenantErrorInfo(TenantErrorCode code) {
    switch (code) {
        case TenantErrorCode::kTenantNotFound:                return kTenantNotFound;
        case TenantErrorCode::kTenantAlreadyExists:           return kTenantAlreadyExists;
        case TenantErrorCode::kTenantEmailDuplicate:          return kTenantEmailDuplicate;
        case TenantErrorCode::kTenantNameDuplicate:           return kTenantNameDuplicate;
        case TenantErrorCode::kTenantDeleteHasNamespaces:     return kTenantDeleteHasNamespaces;
        case TenantErrorCode::kTenantDeleteHasMembers:        return kTenantDeleteHasMembers;
        case TenantErrorCode::kTenantTransferTargetNotMember: return kTenantTransferTargetNotMember;
        case TenantErrorCode::kTenantLastOwnerRemoval:        return kTenantLastOwnerRemoval;
        case TenantErrorCode::kMemberAlreadyExists:           return kMemberAlreadyExists;
        case TenantErrorCode::kMemberNotFound:                return kMemberNotFound;
        case TenantErrorCode::kMemberRoleInvalid:             return kMemberRoleInvalid;
        case TenantErrorCode::kNsUnauthorized:                return kNsUnauthorized;
        case TenantErrorCode::kNsNotFound:                    return kNsNotFound;
        case TenantErrorCode::kAclGrantToOwnerTenant:         return kAclGrantToOwnerTenant;
        case TenantErrorCode::kAclDuplicateGrant:             return kAclDuplicateGrant;
        case TenantErrorCode::kAclGrantInvalidRole:           return kAclGrantInvalidRole;
        case TenantErrorCode::kAclNotFound:                   return kAclNotFound;
        case TenantErrorCode::kPermissionOwnerOnly:           return kPermissionOwnerOnly;
        case TenantErrorCode::kTenantQuotaExceeded:           return kTenantQuotaExceeded;
        case TenantErrorCode::kTenantQuotaNotFound:           return kTenantQuotaNotFound;
        case TenantErrorCode::kTenantTransactionFailed:       return kTenantTransactionFailed;
        case TenantErrorCode::kAdminKeyRequired:              return kAdminKeyRequired;
        case TenantErrorCode::kCallerNotMember:               return kCallerNotMember;
        case TenantErrorCode::kCallerRoleInsufficient:        return kCallerRoleInsufficient;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kTenantNotFound;
}

const char* TenantErrorCodeString(TenantErrorCode code) {
    return GetTenantErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(TenantErrorCode code) {
    // The "structured_data" column, 1:1. Function-local statics -> stable refs.
    static const std::vector<std::string> kTenantId{"tenant_id"};
    static const std::vector<std::string> kTenantIdName{"tenant_id", "name"};
    static const std::vector<std::string> kEmail{"email"};
    static const std::vector<std::string> kNameSuggest{"name", "suggested_alternatives"};
    static const std::vector<std::string> kTenantIdNsCount{"tenant_id", "ns_count", "ns_ids"};
    static const std::vector<std::string> kTenantIdMemberCount{"tenant_id", "member_count"};
    static const std::vector<std::string> kTenantIdTargetUser{"tenant_id", "target_user_id"};
    static const std::vector<std::string> kTenantIdUserId{"tenant_id", "user_id"};
    static const std::vector<std::string> kMemberAlreadyExists{"tenant_id", "user_id", "current_role"};
    static const std::vector<std::string> kRoleValidRoles{"role", "valid_roles"};
    static const std::vector<std::string> kNsUnauthorized{"ns_id", "tenant_id", "user_id", "required_action"};
    static const std::vector<std::string> kNsId{"ns_id"};
    static const std::vector<std::string> kAclGrantToOwner{"ns_id", "tenant_id"};
    static const std::vector<std::string> kAclDuplicate{"ns_id", "grantee_tenant_id", "grantee_user_id", "existing_role"};
    static const std::vector<std::string> kAclNotFound{"ns_id", "grantee_tenant_id", "grantee_user_id"};
    static const std::vector<std::string> kPermissionOwnerOnly{"ns_id", "action", "caller_role"};
    static const std::vector<std::string> kQuotaExceeded{"tenant_id", "quota_type", "current_usage", "quota_limit"};
    static const std::vector<std::string> kQuotaNotFound{"tenant_id", "quota_type"};
    static const std::vector<std::string> kTransactionFailed{"tenant_id", "operation", "db_error"};
    static const std::vector<std::string> kAdminKeyRequired{"endpoint", "required_role"};
    static const std::vector<std::string> kCallerRoleInsufficient{"tenant_id", "caller_role", "required_min_role", "action"};

    switch (code) {
        case TenantErrorCode::kTenantNotFound:                return kTenantId;
        case TenantErrorCode::kTenantAlreadyExists:           return kTenantIdName;
        case TenantErrorCode::kTenantEmailDuplicate:          return kEmail;
        case TenantErrorCode::kTenantNameDuplicate:           return kNameSuggest;
        case TenantErrorCode::kTenantDeleteHasNamespaces:     return kTenantIdNsCount;
        case TenantErrorCode::kTenantDeleteHasMembers:        return kTenantIdMemberCount;
        case TenantErrorCode::kTenantTransferTargetNotMember: return kTenantIdTargetUser;
        case TenantErrorCode::kTenantLastOwnerRemoval:        return kTenantIdUserId;
        case TenantErrorCode::kMemberAlreadyExists:           return kMemberAlreadyExists;
        case TenantErrorCode::kMemberNotFound:                return kTenantIdUserId;
        case TenantErrorCode::kMemberRoleInvalid:             return kRoleValidRoles;
        case TenantErrorCode::kNsUnauthorized:                return kNsUnauthorized;
        case TenantErrorCode::kNsNotFound:                    return kNsId;
        case TenantErrorCode::kAclGrantToOwnerTenant:         return kAclGrantToOwner;
        case TenantErrorCode::kAclDuplicateGrant:             return kAclDuplicate;
        case TenantErrorCode::kAclGrantInvalidRole:           return kRoleValidRoles;
        case TenantErrorCode::kAclNotFound:                   return kAclNotFound;
        case TenantErrorCode::kPermissionOwnerOnly:           return kPermissionOwnerOnly;
        case TenantErrorCode::kTenantQuotaExceeded:           return kQuotaExceeded;
        case TenantErrorCode::kTenantQuotaNotFound:           return kQuotaNotFound;
        case TenantErrorCode::kTenantTransactionFailed:       return kTransactionFailed;
        case TenantErrorCode::kAdminKeyRequired:              return kAdminKeyRequired;
        case TenantErrorCode::kCallerNotMember:               return kTenantIdUserId;
        case TenantErrorCode::kCallerRoleInsufficient:        return kCallerRoleInsufficient;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;
}

bool HasRequiredStructuredData(TenantErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const auto& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeTenantError(TenantErrorCode code,
                                   nlohmann::json structured_data,
                                   const std::string& message) {
    const TenantErrorInfo& info = GetTenantErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode TenantErrorToStatusCode(TenantErrorCode code) {
    switch (code) {
        // not-found family
        case TenantErrorCode::kTenantNotFound:
        case TenantErrorCode::kTenantQuotaNotFound:
        case TenantErrorCode::kMemberNotFound:
        case TenantErrorCode::kNsNotFound:
        case TenantErrorCode::kAclNotFound:
            return StatusCode::kNotFound;
        // already-exists family
        case TenantErrorCode::kTenantAlreadyExists:
        case TenantErrorCode::kTenantEmailDuplicate:
        case TenantErrorCode::kTenantNameDuplicate:
        case TenantErrorCode::kMemberAlreadyExists:
        case TenantErrorCode::kAclDuplicateGrant:
            return StatusCode::kAlreadyExists;
        // permission / auth family
        case TenantErrorCode::kNsUnauthorized:
        case TenantErrorCode::kPermissionOwnerOnly:
        case TenantErrorCode::kCallerNotMember:
        case TenantErrorCode::kCallerRoleInsufficient:
            return StatusCode::kPermissionDenied;
        case TenantErrorCode::kAdminKeyRequired:
            return StatusCode::kUnauthenticated;
        // invalid-argument family (validation / business-rule violation)
        case TenantErrorCode::kTenantDeleteHasNamespaces:
        case TenantErrorCode::kTenantDeleteHasMembers:
        case TenantErrorCode::kTenantTransferTargetNotMember:
        case TenantErrorCode::kTenantLastOwnerRemoval:
        case TenantErrorCode::kMemberRoleInvalid:
        case TenantErrorCode::kAclGrantToOwnerTenant:
        case TenantErrorCode::kAclGrantInvalidRole:
        case TenantErrorCode::kTenantQuotaExceeded:
            return StatusCode::kInvalidArgument;
        // transient
        case TenantErrorCode::kTenantTransactionFailed:
            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;
}

Status TenantStatus(TenantErrorCode code, const std::string& detail) {
    // Prefix the message with the CX_ERR_* token so the exact identity is
    // recoverable at the API/SDK boundary (mirrors CatalogStatus / AuthStatus).
    std::string msg = TenantErrorCodeString(code);
    if (!detail.empty()) {
        msg += ": ";
        msg += detail;
    }
    return Status(TenantErrorToStatusCode(code), msg);
}

}  // namespace cortrix::tenant
