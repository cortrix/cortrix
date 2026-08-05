#include "cortrix/scoring/scoring_error.h"

#include <utility>

namespace cortrix::scoring {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics so each
// returns a stable reference. The switch in GetScoringErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without a
// row" into a warning (treated as a build failure), so the registry can't silently
// drift from the enum.
//
// §4.4: both codes are permanent + non-retryable (retry_after_ms = null). LEVEL_INVALID
// is a defensive bottom-out (ComputeLevel should never exceed 4); CONFIG_INVALID is a
// startup-time bad-α rejection (not part of a query response body).
constexpr ScoringErrorInfo kLevelInvalid
    {"CX_ERR_SCORING_LEVEL_INVALID",  ErrorCategory::kPermanent, false, std::nullopt};
constexpr ScoringErrorInfo kConfigInvalid
    {"CX_ERR_SCORING_CONFIG_INVALID", ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const ScoringErrorInfo& GetScoringErrorInfo(ScoringErrorCode code) {
    switch (code) {
        case ScoringErrorCode::kLevelInvalid:  return kLevelInvalid;
        case ScoringErrorCode::kConfigInvalid: return kConfigInvalid;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function total.
    return kLevelInvalid;
}

const char* ScoringErrorCodeString(ScoringErrorCode code) {
    return GetScoringErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(ScoringErrorCode code) {
    // §4.4 structured_data column — the keys the Agent needs to locate the problem
    // without parsing message. Function-local statics → stable refs.
    static const std::vector<std::string> kLevelKeys{
        "level_received", "max_allowed", "scoring_input"};
    static const std::vector<std::string> kConfigKeys{
        "alpha_received", "valid_range", "config_source"};
    switch (code) {
        case ScoringErrorCode::kLevelInvalid:  return kLevelKeys;
        case ScoringErrorCode::kConfigInvalid: return kConfigKeys;
    }
    static const std::vector<std::string> kEmpty{};
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(ScoringErrorCode code,
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

AgentFriendlyError MakeScoringError(ScoringErrorCode code,
                                    nlohmann::json structured_data,
                                    const std::string& message) {
    const ScoringErrorInfo& info = GetScoringErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode ScoringErrorToStatusCode(ScoringErrorCode code) {
    switch (code) {
        // both are caller/config programming errors → kInvalidArgument (closest coarse
        // code; rich category=permanent preserved via the CX_ERR_ token + boundary
        // MakeScoringError).
        case ScoringErrorCode::kLevelInvalid:
        case ScoringErrorCode::kConfigInvalid: return StatusCode::kInvalidArgument;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status ScoringStatus(ScoringErrorCode code, const std::string& detail) {
    const char* cx = ScoringErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(ScoringErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::scoring
