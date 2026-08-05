#include "cortrix/spc/cleaning_errors.h"

#include <utility>

namespace cortrix::spc {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. constexpr statics → stable references.
// The switch in GetCleaningErrorInfo is intentionally exhaustive: -Wswitch turns
// "added a code without a row" into a build failure, so the registry can't drift
// from the enum.
constexpr CleaningErrorInfo kDedupVectorInvalid  {"CX_ERR_CLEANING_DEDUP_VECTOR_INVALID",  ErrorCategory::kPermanent, false, std::nullopt};
constexpr CleaningErrorInfo kDedupThresholdRange {"CX_ERR_CLEANING_DEDUP_THRESHOLD_RANGE", ErrorCategory::kPermanent, false, std::nullopt};
constexpr CleaningErrorInfo kAnomalyConfigInvalid{"CX_ERR_CLEANING_ANOMALY_CONFIG_INVALID",ErrorCategory::kPermanent, false, std::nullopt};
constexpr CleaningErrorInfo kPluginTimeout       {"CX_ERR_CLEANING_PLUGIN_TIMEOUT",        ErrorCategory::kTimeout,   true,  1000};
constexpr CleaningErrorInfo kPluginException     {"CX_ERR_CLEANING_PLUGIN_EXCEPTION",      ErrorCategory::kPermanent, false, std::nullopt};
constexpr CleaningErrorInfo kNsConfigMergeFailed {"CX_ERR_CLEANING_NS_CONFIG_MERGE_FAILED",ErrorCategory::kPermanent, false, std::nullopt};
constexpr CleaningErrorInfo kInternalError       {"CX_ERR_CLEANING_INTERNAL_ERROR",        ErrorCategory::kTransient, true,  2000};

}  // namespace

const CleaningErrorInfo& GetCleaningErrorInfo(CleaningErrorCode code) {
    switch (code) {
        case CleaningErrorCode::kDedupVectorInvalid:   return kDedupVectorInvalid;
        case CleaningErrorCode::kDedupThresholdRange:  return kDedupThresholdRange;
        case CleaningErrorCode::kAnomalyConfigInvalid: return kAnomalyConfigInvalid;
        case CleaningErrorCode::kPluginTimeout:        return kPluginTimeout;
        case CleaningErrorCode::kPluginException:      return kPluginException;
        case CleaningErrorCode::kNsConfigMergeFailed:  return kNsConfigMergeFailed;
        case CleaningErrorCode::kInternalError:        return kInternalError;
    }
    return kInternalError;  // defensive; keeps the function total
}

const char* CleaningErrorCodeString(CleaningErrorCode code) {
    return GetCleaningErrorInfo(code).cx_code;
}

AgentFriendlyError MakeCleaningError(CleaningErrorCode code,
                                     nlohmann::json structured_data,
                                     const std::string& message) {
    const CleaningErrorInfo& info = GetCleaningErrorInfo(code);
    if (structured_data.is_object() && !structured_data.contains("code")) {
        structured_data["code"] = info.cx_code;
    }
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode CleaningErrorToStatusCode(CleaningErrorCode code) {
    switch (code) {
        case CleaningErrorCode::kDedupVectorInvalid:
        case CleaningErrorCode::kDedupThresholdRange:
        case CleaningErrorCode::kAnomalyConfigInvalid:
            return StatusCode::kInvalidArgument;
        case CleaningErrorCode::kPluginTimeout:
        case CleaningErrorCode::kInternalError:
            return StatusCode::kUnavailable;
        case CleaningErrorCode::kPluginException:
        case CleaningErrorCode::kNsConfigMergeFailed:
            return StatusCode::kInternal;
    }
    return StatusCode::kInternal;
}

Status CleaningStatus(CleaningErrorCode code, const std::string& detail) {
    const char* cx = CleaningErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(CleaningErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::spc
