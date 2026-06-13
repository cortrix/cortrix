#include "cortrix/resource/pool_error.h"

#include <utility>

namespace cortrix::resource {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Function-local statics → stable references. The
// switches below are intentionally exhaustive: -Wswitch turns "added a code
// without a row" into a build failure, so the registry can't drift from the enum.
//
// The 3 admission codes reuse the same CX_ERR_NS_* strings + attributes that
// catalog_error.cpp registers (shared NS semantic domain, §8.1 m4) — they are
// the same wire identities, seen from the F05 side.
constexpr PoolErrorInfo kNsQuotaExceeded     {"CX_ERR_NS_QUOTA_EXCEEDED",           ErrorCategory::kQuota,     false, std::nullopt};
constexpr PoolErrorInfo kNsResourceBudget    {"CX_ERR_NS_RESOURCE_BUDGET_EXCEEDED", ErrorCategory::kQuota,     false, std::nullopt};
constexpr PoolErrorInfo kNsLoadFailed        {"CX_ERR_NS_LOAD_FAILED",              ErrorCategory::kTransient, true,  1000};
constexpr PoolErrorInfo kNsNotFound          {"CX_ERR_NS_NOT_FOUND",                ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const PoolErrorInfo& GetPoolErrorInfo(PoolErrorCode code) {
    switch (code) {
        case PoolErrorCode::kNsQuotaExceeded:          return kNsQuotaExceeded;
        case PoolErrorCode::kNsResourceBudgetExceeded: return kNsResourceBudget;
        case PoolErrorCode::kNsLoadFailed:             return kNsLoadFailed;
        case PoolErrorCode::kNsNotFound:               return kNsNotFound;
    }
    return kNsLoadFailed;  // unreachable for a valid enum; keeps the fn total
}

const char* PoolErrorCodeString(PoolErrorCode code) {
    return GetPoolErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(PoolErrorCode code) {
    // F05 §8.1 "structured_data required keys" column, 1:1.
    static const std::vector<std::string> kQuota{"current_count", "limit", "namespace_id"};
    static const std::vector<std::string> kBudget{"budget_used", "budget_limit",
                                                  "estimated_size", "namespace_id"};
    static const std::vector<std::string> kLoad{"namespace_id", "load_step", "error_detail"};
    static const std::vector<std::string> kNotFound{"namespace_id"};

    switch (code) {
        case PoolErrorCode::kNsQuotaExceeded:          return kQuota;
        case PoolErrorCode::kNsResourceBudgetExceeded: return kBudget;
        case PoolErrorCode::kNsLoadFailed:             return kLoad;
        case PoolErrorCode::kNsNotFound:               return kNotFound;
    }
    return kNotFound;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(PoolErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakePoolError(PoolErrorCode code,
                                 nlohmann::json structured_data,
                                 const std::string& message) {
    const PoolErrorInfo& info = GetPoolErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode PoolErrorToStatusCode(PoolErrorCode code) {
    switch (code) {
        // quota → kPermissionDenied (closest coarse code; rich category=quota is
        // preserved via the CX_ERR_ token + boundary MakePoolError). Matches the
        // catalog_error.cpp mapping for the same shared CX_ERR_NS_* codes.
        case PoolErrorCode::kNsQuotaExceeded:
        case PoolErrorCode::kNsResourceBudgetExceeded:
            return StatusCode::kPermissionDenied;
        // transient / retryable → kUnavailable.
        case PoolErrorCode::kNsLoadFailed:
            return StatusCode::kUnavailable;
        // not found → kNotFound.
        case PoolErrorCode::kNsNotFound:
            return StatusCode::kNotFound;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status PoolStatus(PoolErrorCode code, const std::string& detail) {
    const char* cx = PoolErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(PoolErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::resource
