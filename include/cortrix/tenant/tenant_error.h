#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::tenant {

/// The 24 tenant/permission/quota error identities. Each maps to a
/// stable `CX_ERR_*` string + a GEN-Agent category + retryability via the
/// canonical registry below.
///
/// Per CODING_CONVENTIONS sec 3, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// So TenantErrorCode is the *enum of identities*, and MakeTenantError() turns one
/// (plus optional structured_data) into that boundary error -- we do NOT introduce
/// a parallel TenantError struct (the sec 4.1 `TenantError` / `Result<T,TenantError>`
/// spelling is the pre-convention draft; this is its convention-compliant form,
/// exactly as catalog_error.h / auth_error.h handle the same shape).
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class TenantErrorCode {
    // --- Tenant CRUD ---
    kTenantNotFound,                  // CX_ERR_TENANT_NOT_FOUND
    kTenantAlreadyExists,             // CX_ERR_TENANT_ALREADY_EXISTS
    kTenantEmailDuplicate,            // CX_ERR_TENANT_EMAIL_DUPLICATE
    kTenantNameDuplicate,             // CX_ERR_TENANT_NAME_DUPLICATE
    kTenantDeleteHasNamespaces,       // CX_ERR_TENANT_DELETE_HAS_NAMESPACES
    kTenantDeleteHasMembers,          // CX_ERR_TENANT_DELETE_HAS_MEMBERS
    kTenantTransferTargetNotMember,   // CX_ERR_TENANT_TRANSFER_TARGET_NOT_MEMBER
    kTenantLastOwnerRemoval,          // CX_ERR_TENANT_LAST_OWNER_REMOVAL
    // --- Member ---
    kMemberAlreadyExists,             // CX_ERR_MEMBER_ALREADY_EXISTS
    kMemberNotFound,                  // CX_ERR_MEMBER_NOT_FOUND
    kMemberRoleInvalid,               // CX_ERR_MEMBER_ROLE_INVALID
    // --- Namespace / permission ---
    kNsUnauthorized,                  // CX_ERR_NS_UNAUTHORIZED
    kNsNotFound,                      // CX_ERR_NS_NOT_FOUND
    // --- ACL ---
    kAclGrantToOwnerTenant,           // CX_ERR_ACL_GRANT_TO_OWNER_TENANT
    kAclDuplicateGrant,               // CX_ERR_ACL_DUPLICATE_GRANT
    kAclGrantInvalidRole,             // CX_ERR_ACL_GRANT_INVALID_ROLE
    kAclNotFound,                     // CX_ERR_ACL_NOT_FOUND
    kPermissionOwnerOnly,             // CX_ERR_PERMISSION_OWNER_ONLY
    // --- Quota ---
    kTenantQuotaExceeded,             // CX_ERR_TENANT_QUOTA_EXCEEDED
    kTenantQuotaNotFound,             // CX_ERR_TENANT_QUOTA_NOT_FOUND
    // --- Transaction / auth ---
    kTenantTransactionFailed,         // CX_ERR_TENANT_TRANSACTION_FAILED (retryable)
    kAdminKeyRequired,                // CX_ERR_ADMIN_KEY_REQUIRED
    kCallerNotMember,                 // CX_ERR_CALLER_NOT_MEMBER
    kCallerRoleInsufficient,          // CX_ERR_CALLER_ROLE_INSUFFICIENT
};

/// Total number of tenant error codes (= 24). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kTenantErrorCodeCount = 24;

/// Canonical, immutable attributes of one error code.
struct TenantErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    agent_friendly::ErrorCategory category;   ///< auth/quota/transient/permanent/timeout
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per sec 7
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 24 rows.
const TenantErrorInfo& GetTenantErrorInfo(TenantErrorCode code);

/// The "CX_ERR_*" string for `code` (convenience over GetTenantErrorInfo).
const char* TenantErrorCodeString(TenantErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry
/// (structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5);
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(TenantErrorCode code);

/// True iff `structured_data` contains every required key for `code`. Used to
/// assert Agent-friendly completeness before returning an error body.
bool HasRequiredStructuredData(TenantErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// (the sec 7 required keys are the caller's responsibility at each call site) and an
/// optional human-readable `message`. category / retryable / retry_after_ms are
/// filled from the canonical registry -- call sites never restate them.
agent_friendly::AgentFriendlyError MakeTenantError(
    TenantErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge a tenant error to a plain Status for the Result<T>/Status interface
/// surface. The StatusCode is the coarse mapping of `code`; the message is
/// prefixed with the CX_ERR_* token ("CX_ERR_X: detail") so the exact tenant
/// identity is recoverable at the API/SDK boundary, which re-inflates the full
/// Agent-friendly body via MakeTenantError(). Mirrors catalog::CatalogStatus /
/// auth::AuthStatus -- we deliberately do NOT widen cortrix::Status itself.
Status TenantStatus(TenantErrorCode code, const std::string& detail = "");

/// Coarse TenantErrorCode -> StatusCode mapping used by TenantStatus (exposed for
/// tests / boundary code that needs the mapping without a message).
StatusCode TenantErrorToStatusCode(TenantErrorCode code);

}  // namespace cortrix::tenant
