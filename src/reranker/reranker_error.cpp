#include "cortrix/reranker/reranker_error.h"

#include <utility>

namespace cortrix::reranker {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so each
// returns a stable reference. The switches below are intentionally exhaustive:
// building with -Wall -Wextra (-Wswitch) turns "added a code without a row" into a
// warning, which the project treats as a build failure — the registry can't
// silently drift from the enum.
//
// retry_after_ms (§5.2 table): TASK_TIMEOUT → 5000 (== default task_timeout_ms);
// CIRCUIT_OPEN → 30000 (== default cooldown_sec); the two startup errors are
// permanent (user must fix config / model), CHUNK_NOT_FOUND is permanent.
constexpr RerankerErrorInfo kInitFailed{
    "CX_ERR_RERANKER_INIT_FAILED", ErrorCategory::kPermanent, false, std::nullopt};
constexpr RerankerErrorInfo kConfigMismatch{
    "CX_ERR_CONFIG_MISMATCH", ErrorCategory::kPermanent, false, std::nullopt};
constexpr RerankerErrorInfo kTaskTimeout{
    "CX_ERR_RERANKER_TASK_TIMEOUT", ErrorCategory::kTimeout, true, 5000};
constexpr RerankerErrorInfo kChunkNotFound{
    "CX_ERR_CHUNK_NOT_FOUND", ErrorCategory::kPermanent, false, std::nullopt};
constexpr RerankerErrorInfo kCircuitOpen{
    "CX_ERR_RERANKER_CIRCUIT_OPEN", ErrorCategory::kTransient, true, 30000};

}  // namespace

const RerankerErrorInfo& GetRerankerErrorInfo(RerankerErrorCode code) {
    switch (code) {
        case RerankerErrorCode::kRerankerInitFailed:  return kInitFailed;
        case RerankerErrorCode::kConfigMismatch:      return kConfigMismatch;
        case RerankerErrorCode::kRerankerTaskTimeout: return kTaskTimeout;
        case RerankerErrorCode::kChunkNotFound:       return kChunkNotFound;
        case RerankerErrorCode::kRerankerCircuitOpen: return kCircuitOpen;
    }
    return kInitFailed;  // unreachable for a valid enum value
}

const char* RerankerErrorCodeString(RerankerErrorCode code) {
    return GetRerankerErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(RerankerErrorCode code) {
    // §5.2 structured_data column, 1:1. Function-local statics → stable references.
    static const std::vector<std::string> kInit{"model_path", "ep"};
    static const std::vector<std::string> kCfg{"chunk_size", "max_seq_length"};
    static const std::vector<std::string> kTimeout{"child_id", "timeout_ms"};
    static const std::vector<std::string> kChunk{"child_id"};
    static const std::vector<std::string> kCircuit{"open_since", "failure_count"};

    switch (code) {
        case RerankerErrorCode::kRerankerInitFailed:  return kInit;
        case RerankerErrorCode::kConfigMismatch:      return kCfg;
        case RerankerErrorCode::kRerankerTaskTimeout: return kTimeout;
        case RerankerErrorCode::kChunkNotFound:       return kChunk;
        case RerankerErrorCode::kRerankerCircuitOpen: return kCircuit;
    }
    return kInit;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(RerankerErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeRerankerError(RerankerErrorCode code,
                                     nlohmann::json structured_data,
                                     const std::string& message) {
    const RerankerErrorInfo& info = GetRerankerErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode RerankerErrorToStatusCode(RerankerErrorCode code) {
    switch (code) {
        // Startup config/model errors are permanent, caller-side (bad config /
        // missing model) — not a transient server fault → kInvalidArgument. The
        // rich CX_ERR_* identity + category survive via the token + boundary
        // MakeRerankerError().
        case RerankerErrorCode::kRerankerInitFailed:
        case RerankerErrorCode::kConfigMismatch:
            return StatusCode::kInvalidArgument;
        // Missing chunk → kNotFound (matches the frozen ChunkStore contract).
        case RerankerErrorCode::kChunkNotFound:
            return StatusCode::kNotFound;
        // Timeout / circuit-open are transient / retryable → kUnavailable.
        case RerankerErrorCode::kRerankerTaskTimeout:
        case RerankerErrorCode::kRerankerCircuitOpen:
            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status RerankerStatus(RerankerErrorCode code, const std::string& detail) {
    const char* cx = RerankerErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(RerankerErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::reranker
