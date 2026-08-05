#include "cortrix/deploy/deploy_error.h"

#include <utility>

namespace cortrix::deploy {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so each
// returns a stable reference. The switch in GetDeployErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without
// a row" into a warning (treated as a build failure), so the registry can't
// silently drift from the enum.
//
// http_status / category / retryable / retry_after_ms follow §12 exactly:
//   507 DISK_FULL              permanent  false  null   (out of storage)
//   503 SHUTDOWN_IN_PROGRESS   transient  true   5000   (retry after restart)
//   503 HEALTH_CHECK_FAILED    transient  true   1000
//   503 BF_NOT_READY           transient  true   2000   (retry after rebuild)
constexpr DeployErrorInfo kDiskFull
    {"CX_ERR_DISK_FULL",              507, ErrorCategory::kPermanent, false, std::nullopt};
constexpr DeployErrorInfo kShutdownInProgress
    {"CX_ERR_SHUTDOWN_IN_PROGRESS",   503, ErrorCategory::kTransient, true,  5000};
constexpr DeployErrorInfo kHealthCheckFailed
    {"CX_ERR_HEALTH_CHECK_FAILED",    503, ErrorCategory::kTransient, true,  1000};
constexpr DeployErrorInfo kBfNotReady
    {"CX_ERR_BF_NOT_READY",           503, ErrorCategory::kTransient, true,  2000};

}  // namespace

const DeployErrorInfo& GetDeployErrorInfo(DeployErrorCode code) {
    switch (code) {
        case DeployErrorCode::kDiskFull:            return kDiskFull;
        case DeployErrorCode::kShutdownInProgress:  return kShutdownInProgress;
        case DeployErrorCode::kHealthCheckFailed:   return kHealthCheckFailed;
        case DeployErrorCode::kBfNotReady:          return kBfNotReady;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kDiskFull;
}

const char* DeployErrorCodeString(DeployErrorCode code) {
    return GetDeployErrorInfo(code).cx_code;
}

int DeployErrorHttpStatus(DeployErrorCode code) {
    return GetDeployErrorInfo(code).http_status;
}

const std::vector<std::string>& RequiredStructuredDataKeys(DeployErrorCode code) {
    // §6.3 / §12 structured_data column. Function-local statics → stable refs.
    static const std::vector<std::string> kDiskFullKeys
        {"disk_usage_ratio", "free_bytes", "data_path", "warn_threshold", "crit_threshold"};
    static const std::vector<std::string> kShutdownKeys
        {"shutdown_status"};  // grace_remaining_ms is optional (§12 marks it "?")
    static const std::vector<std::string> kHealthKeys
        {"component", "error_id"};
    static const std::vector<std::string> kBfKeys
        {"rebuild_progress"};

    switch (code) {
        case DeployErrorCode::kDiskFull:            return kDiskFullKeys;
        case DeployErrorCode::kShutdownInProgress:  return kShutdownKeys;
        case DeployErrorCode::kHealthCheckFailed:   return kHealthKeys;
        case DeployErrorCode::kBfNotReady:          return kBfKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(DeployErrorCode code,
                               const nlohmann::json& structured_data) {
    const std::vector<std::string>& keys = RequiredStructuredDataKeys(code);
    if (!structured_data.is_object()) {
        return keys.empty();
    }
    for (const std::string& key : keys) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeDeployError(DeployErrorCode code,
                                nlohmann::json structured_data,
                                const std::string& message) {
    const DeployErrorInfo& info = GetDeployErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode DeployErrorToStatusCode(DeployErrorCode code) {
    switch (code) {
        // out of storage → kUnavailable (closest coarse code; the rich permanent
        // category + CX_ERR_DISK_FULL token re-inflate at the API boundary).
        case DeployErrorCode::kDiskFull:            return StatusCode::kUnavailable;
        // shutting down / health self-error / BF rebuilding → kUnavailable
        // (transient, retryable).
        case DeployErrorCode::kShutdownInProgress:
        case DeployErrorCode::kHealthCheckFailed:
        case DeployErrorCode::kBfNotReady:          return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status DeployStatus(DeployErrorCode code, const std::string& detail) {
    const char* cx = DeployErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(DeployErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::deploy
