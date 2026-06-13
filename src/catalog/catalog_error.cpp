#include "cortrix/catalog/catalog_error.h"

#include <utility>

namespace cortrix::catalog {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so each returns a
// stable reference. The switch is intentionally exhaustive: building with
// -Wall -Wextra (-Wswitch) turns "added a code without a row" into a warning,
// which the project treats as a build failure — the registry can't silently drift
// from the enum.
constexpr CatalogErrorInfo kNsNotFound          {"CX_ERR_NS_NOT_FOUND",            ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kNsUnauthorized      {"CX_ERR_NS_UNAUTHORIZED",         ErrorCategory::kAuth,      false, std::nullopt};
constexpr CatalogErrorInfo kNsIsolated          {"CX_ERR_NS_ISOLATED",             ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kNsVisibilityDenied  {"CX_ERR_NS_VISIBILITY_DENIED",    ErrorCategory::kAuth,      false, std::nullopt};
constexpr CatalogErrorInfo kNsAlreadyExists     {"CX_ERR_NS_ALREADY_EXISTS",       ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kUnitNotFound        {"CX_ERR_UNIT_NOT_FOUND",          ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kUnitNotSealed       {"CX_ERR_UNIT_NOT_SEALED",         ErrorCategory::kTransient, true,  5000};
constexpr CatalogErrorInfo kUnitArchived        {"CX_ERR_UNIT_ARCHIVED",           ErrorCategory::kTransient, true,  10000};
constexpr CatalogErrorInfo kUnitOwnerRemote     {"CX_ERR_UNIT_OWNER_REMOTE",       ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kTenantNotFound      {"CX_ERR_TENANT_NOT_FOUND",        ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kTenantQuotaExceeded {"CX_ERR_TENANT_QUOTA_EXCEEDED",   ErrorCategory::kQuota,     false, std::nullopt};
constexpr CatalogErrorInfo kSchemaVersionMismatch{"CX_ERR_SCHEMA_VERSION_MISMATCH", ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kSchemaMigrationFailed{"CX_ERR_SCHEMA_MIGRATION_FAILED", ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kInvalidConfigJson   {"CX_ERR_INVALID_CONFIG_JSON",     ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kCatalogLocked       {"CX_ERR_CATALOG_LOCKED",          ErrorCategory::kTransient, true,  100};
constexpr CatalogErrorInfo kBloomFilterNotReady {"CX_ERR_BLOOM_FILTER_NOT_READY",  ErrorCategory::kTransient, true,  1000};
constexpr CatalogErrorInfo kTransactionRetry    {"CX_ERR_TRANSACTION_RETRY",       ErrorCategory::kTransient, true,  50};
constexpr CatalogErrorInfo kContentHashMismatch {"CX_ERR_CONTENT_HASH_MISMATCH",   ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kRefCountNegative    {"CX_ERR_REF_COUNT_NEGATIVE",      ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kInternalError       {"CX_ERR_INTERNAL_ERROR",          ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kNotImplemented      {"CX_ERR_NOT_IMPLEMENTED",         ErrorCategory::kPermanent, false, std::nullopt};
constexpr CatalogErrorInfo kNsQuotaExceeded     {"CX_ERR_NS_QUOTA_EXCEEDED",       ErrorCategory::kQuota,     false, std::nullopt};
constexpr CatalogErrorInfo kNsResourceBudget    {"CX_ERR_NS_RESOURCE_BUDGET_EXCEEDED", ErrorCategory::kQuota, false, std::nullopt};
constexpr CatalogErrorInfo kNsPoolInternal      {"CX_ERR_NS_POOL_INTERNAL",        ErrorCategory::kTransient, true,  5000};

}  // namespace

const CatalogErrorInfo& GetCatalogErrorInfo(CatalogErrorCode code) {
    switch (code) {
        case CatalogErrorCode::kNsNotFound:             return kNsNotFound;
        case CatalogErrorCode::kNsUnauthorized:         return kNsUnauthorized;
        case CatalogErrorCode::kNsIsolated:             return kNsIsolated;
        case CatalogErrorCode::kNsVisibilityDenied:     return kNsVisibilityDenied;
        case CatalogErrorCode::kNsAlreadyExists:        return kNsAlreadyExists;
        case CatalogErrorCode::kUnitNotFound:           return kUnitNotFound;
        case CatalogErrorCode::kUnitNotSealed:          return kUnitNotSealed;
        case CatalogErrorCode::kUnitArchived:           return kUnitArchived;
        case CatalogErrorCode::kUnitOwnerRemote:        return kUnitOwnerRemote;
        case CatalogErrorCode::kTenantNotFound:         return kTenantNotFound;
        case CatalogErrorCode::kTenantQuotaExceeded:    return kTenantQuotaExceeded;
        case CatalogErrorCode::kSchemaVersionMismatch:  return kSchemaVersionMismatch;
        case CatalogErrorCode::kSchemaMigrationFailed:  return kSchemaMigrationFailed;
        case CatalogErrorCode::kInvalidConfigJson:      return kInvalidConfigJson;
        case CatalogErrorCode::kCatalogLocked:          return kCatalogLocked;
        case CatalogErrorCode::kBloomFilterNotReady:    return kBloomFilterNotReady;
        case CatalogErrorCode::kTransactionRetry:       return kTransactionRetry;
        case CatalogErrorCode::kContentHashMismatch:    return kContentHashMismatch;
        case CatalogErrorCode::kRefCountNegative:       return kRefCountNegative;
        case CatalogErrorCode::kInternalError:          return kInternalError;
        case CatalogErrorCode::kNotImplemented:         return kNotImplemented;
        case CatalogErrorCode::kNsQuotaExceeded:        return kNsQuotaExceeded;
        case CatalogErrorCode::kNsResourceBudgetExceeded: return kNsResourceBudget;
        case CatalogErrorCode::kNsPoolInternal:         return kNsPoolInternal;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kInternalError;
}

const char* CatalogErrorCodeString(CatalogErrorCode code) {
    return GetCatalogErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(CatalogErrorCode code) {
    // §8.1 "structured_data required keys" column, 1:1. Function-local statics → stable
    // references. kInternalError is "by scenario" (case-by-case) → no fixed keys.
    static const std::vector<std::string> kEmpty{};
    static const std::vector<std::string> kNsId{"ns_id"};
    static const std::vector<std::string> kNsUnauthorized{"ns_id", "tenant_id", "user_id", "required_permission"};
    static const std::vector<std::string> kNsIsolated{"ns_id", "isolation_mode"};
    static const std::vector<std::string> kNsVisibility{"ns_id", "visibility", "required_visibility"};
    static const std::vector<std::string> kUnitNsId{"unit_id", "ns_id"};
    static const std::vector<std::string> kUnitState{"unit_id", "current_state"};
    static const std::vector<std::string> kUnitId{"unit_id"};
    static const std::vector<std::string> kUnitOwner{"unit_id", "owner_node_id", "current_node_id"};
    static const std::vector<std::string> kTenantId{"tenant_id"};
    static const std::vector<std::string> kTenantQuota{"tenant_id", "quota_type", "current_usage", "quota_limit"};
    static const std::vector<std::string> kSchemaVer{"feature_name", "expected_version", "current_version"};
    static const std::vector<std::string> kSchemaMig{"feature_name", "migration_step", "error_detail"};
    static const std::vector<std::string> kInvalidCfg{"ns_id", "config_field", "parse_error"};
    static const std::vector<std::string> kCatalogLocked{"lock_holder", "wait_timeout_ms"};
    static const std::vector<std::string> kBloom{"load_progress", "estimated_ready_ms"};
    static const std::vector<std::string> kTxnRetry{"sqlite_errcode"};
    static const std::vector<std::string> kHashMismatch{"file_hash", "catalog_hash", "block_hash"};
    static const std::vector<std::string> kRefNeg{"file_hash", "ns_id", "current_ref_count"};
    static const std::vector<std::string> kNotImpl{"feature"};
    static const std::vector<std::string> kNsQuota{"current_count", "limit", "namespace_id"};
    static const std::vector<std::string> kNsBudget{"budget_used", "budget_limit", "estimated_size", "namespace_id"};
    static const std::vector<std::string> kNsPool{"namespace_id", "pool_error_detail"};

    switch (code) {
        case CatalogErrorCode::kNsNotFound:             return kNsId;
        case CatalogErrorCode::kNsUnauthorized:         return kNsUnauthorized;
        case CatalogErrorCode::kNsIsolated:             return kNsIsolated;
        case CatalogErrorCode::kNsVisibilityDenied:     return kNsVisibility;
        case CatalogErrorCode::kNsAlreadyExists:        return kNsId;
        case CatalogErrorCode::kUnitNotFound:           return kUnitNsId;
        case CatalogErrorCode::kUnitNotSealed:          return kUnitState;
        case CatalogErrorCode::kUnitArchived:           return kUnitId;
        case CatalogErrorCode::kUnitOwnerRemote:        return kUnitOwner;
        case CatalogErrorCode::kTenantNotFound:         return kTenantId;
        case CatalogErrorCode::kTenantQuotaExceeded:    return kTenantQuota;
        case CatalogErrorCode::kSchemaVersionMismatch:  return kSchemaVer;
        case CatalogErrorCode::kSchemaMigrationFailed:  return kSchemaMig;
        case CatalogErrorCode::kInvalidConfigJson:      return kInvalidCfg;
        case CatalogErrorCode::kCatalogLocked:          return kCatalogLocked;
        case CatalogErrorCode::kBloomFilterNotReady:    return kBloom;
        case CatalogErrorCode::kTransactionRetry:       return kTxnRetry;
        case CatalogErrorCode::kContentHashMismatch:    return kHashMismatch;
        case CatalogErrorCode::kRefCountNegative:       return kRefNeg;
        case CatalogErrorCode::kInternalError:          return kEmpty;   // by scenario
        case CatalogErrorCode::kNotImplemented:         return kNotImpl;
        case CatalogErrorCode::kNsQuotaExceeded:        return kNsQuota;
        case CatalogErrorCode::kNsResourceBudgetExceeded: return kNsBudget;
        case CatalogErrorCode::kNsPoolInternal:         return kNsPool;
    }
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(CatalogErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeCatalogError(CatalogErrorCode code,
                                    nlohmann::json structured_data,
                                    const std::string& message) {
    const CatalogErrorInfo& info = GetCatalogErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode CatalogErrorToStatusCode(CatalogErrorCode code) {
    switch (code) {
        // NS/Unit/Tenant "not found" → kNotFound.
        case CatalogErrorCode::kNsNotFound:
        case CatalogErrorCode::kUnitNotFound:
        case CatalogErrorCode::kTenantNotFound:
            return StatusCode::kNotFound;
        // "already exists" → kAlreadyExists.
        case CatalogErrorCode::kNsAlreadyExists:
            return StatusCode::kAlreadyExists;
        // auth-category codes → kPermissionDenied.
        case CatalogErrorCode::kNsUnauthorized:
        case CatalogErrorCode::kNsVisibilityDenied:
            return StatusCode::kPermissionDenied;
        // isolation / config / integrity / not-implemented → kInvalidArgument
        // (caller-side / permanent, non-retryable, not a server fault).
        case CatalogErrorCode::kNsIsolated:
        case CatalogErrorCode::kInvalidConfigJson:
        case CatalogErrorCode::kContentHashMismatch:
        case CatalogErrorCode::kRefCountNegative:
        case CatalogErrorCode::kSchemaVersionMismatch:
        case CatalogErrorCode::kNotImplemented:
            return StatusCode::kInvalidArgument;
        // quota → kPermissionDenied (closest coarse code; rich category=quota
        // is preserved via the CX_ERR_ token + boundary MakeCatalogError).
        case CatalogErrorCode::kTenantQuotaExceeded:
        case CatalogErrorCode::kNsQuotaExceeded:
        case CatalogErrorCode::kNsResourceBudgetExceeded:
            return StatusCode::kPermissionDenied;
        // transient / retryable → kUnavailable.
        case CatalogErrorCode::kUnitNotSealed:
        case CatalogErrorCode::kUnitArchived:
        case CatalogErrorCode::kCatalogLocked:
        case CatalogErrorCode::kBloomFilterNotReady:
        case CatalogErrorCode::kTransactionRetry:
        case CatalogErrorCode::kNsPoolInternal:
            return StatusCode::kUnavailable;
        // remote owner is a permanent routing error for this node.
        case CatalogErrorCode::kUnitOwnerRemote:
            return StatusCode::kInvalidArgument;
        // schema migration / generic internal → kInternal.
        case CatalogErrorCode::kSchemaMigrationFailed:
        case CatalogErrorCode::kInternalError:
            return StatusCode::kInternal;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status CatalogStatus(CatalogErrorCode code, const std::string& detail) {
    const char* cx = CatalogErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(CatalogErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::catalog
