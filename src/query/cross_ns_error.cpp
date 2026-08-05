#include "cortrix/query/cross_ns_error.h"

#include <utility>

namespace cortrix::query {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Defined as function-local statics
// so each returns a stable reference. The switch in GetCrossNsErrorInfo is
// intentionally exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a
// code without a row" into a warning (treated as a build failure), so the registry
// can't silently drift from the enum.
//
// retry_after_ms follows §3.2 Agent decision matrix: NS_TIMEOUT / SCATTER_TIMEOUT
// are retryable=true with 1000ms; everything else is non-retryable (null).
constexpr CrossNsErrorInfo kAuthInvalidCredentials
    {"CX_ERR_AUTH_INVALID_CREDENTIALS", ErrorCategory::kAuth,      false, std::nullopt, 401};
constexpr CrossNsErrorInfo kNsUnauthorized
    {"CX_ERR_NS_UNAUTHORIZED",          ErrorCategory::kAuth,      false, std::nullopt, 403};
constexpr CrossNsErrorInfo kTooManyNamespaces
    {"CX_ERR_TOO_MANY_NAMESPACES",      ErrorCategory::kQuota,     false, std::nullopt, 400};
constexpr CrossNsErrorInfo kNsTimeout
    {"CX_ERR_NS_TIMEOUT",               ErrorCategory::kTimeout,   true,  1000,         200};
constexpr CrossNsErrorInfo kIndexCorrupt
    {"CX_ERR_INDEX_CORRUPT",            ErrorCategory::kPermanent, false, std::nullopt, 200};
constexpr CrossNsErrorInfo kScatterTimeout
    {"CX_ERR_SCATTER_TIMEOUT",          ErrorCategory::kTimeout,   true,  1000,         200};

}  // namespace

const CrossNsErrorInfo& GetCrossNsErrorInfo(CrossNsErrorCode code) {
    switch (code) {
        case CrossNsErrorCode::kAuthInvalidCredentials: return kAuthInvalidCredentials;
        case CrossNsErrorCode::kNsUnauthorized:         return kNsUnauthorized;
        case CrossNsErrorCode::kTooManyNamespaces:      return kTooManyNamespaces;
        case CrossNsErrorCode::kNsTimeout:              return kNsTimeout;
        case CrossNsErrorCode::kIndexCorrupt:           return kIndexCorrupt;
        case CrossNsErrorCode::kScatterTimeout:         return kScatterTimeout;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kIndexCorrupt;
}

const char* CrossNsErrorCodeString(CrossNsErrorCode code) {
    return GetCrossNsErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(CrossNsErrorCode code) {
    // §3.1 / §5 "structured_data required keys". Function-local statics → stable refs.
    static const std::vector<std::string> kEmpty{};
    static const std::vector<std::string> kUnauthorized{"unauthorized_namespaces"};
    static const std::vector<std::string> kTooMany{"requested_count", "max_namespaces"};
    static const std::vector<std::string> kNsId{"namespace"};
    static const std::vector<std::string> kIndexCorrupt{"namespace", "index_state"};

    switch (code) {
        // 401 unauthenticated: body is contextual (no required structured data).
        case CrossNsErrorCode::kAuthInvalidCredentials: return kEmpty;
        // 403: Agent extracts unauthorized_namespaces → re-queries authorized only
        // (§3.2 / GEN-Agent #5).
        case CrossNsErrorCode::kNsUnauthorized:         return kUnauthorized;
        // 400: Agent slices to the cap + parallel re-queries (§3.2).
        case CrossNsErrorCode::kTooManyNamespaces:      return kTooMany;
        // per-NS failures (live in meta.namespaces_failed[]):
        case CrossNsErrorCode::kNsTimeout:              return kNsId;
        case CrossNsErrorCode::kIndexCorrupt:           return kIndexCorrupt;
        // overall scatter timeout: partial results returned; body is contextual.
        case CrossNsErrorCode::kScatterTimeout:         return kEmpty;
    }
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(CrossNsErrorCode code,
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

AgentFriendlyError MakeCrossNsError(CrossNsErrorCode code,
                                    nlohmann::json structured_data,
                                    const std::string& message) {
    const CrossNsErrorInfo& info = GetCrossNsErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

AgentFriendlyError MakeNsUnauthorizedError(
    const std::vector<std::string>& unauthorized_namespaces) {
    nlohmann::json sd = nlohmann::json::object();
    sd["unauthorized_namespaces"] = unauthorized_namespaces;
    return MakeCrossNsError(CrossNsErrorCode::kNsUnauthorized, std::move(sd),
                            "Some namespaces are unauthorized");
}

StatusCode CrossNsErrorToStatusCode(CrossNsErrorCode code) {
    switch (code) {
        case CrossNsErrorCode::kAuthInvalidCredentials: return StatusCode::kUnauthenticated;
        case CrossNsErrorCode::kNsUnauthorized:         return StatusCode::kPermissionDenied;
        // quota → kPermissionDenied (closest coarse code; rich category=quota
        // preserved via the CX_ERR_ token + boundary MakeCrossNsError).
        case CrossNsErrorCode::kTooManyNamespaces:      return StatusCode::kPermissionDenied;
        // timeouts / transient corruption → kUnavailable.
        case CrossNsErrorCode::kNsTimeout:
        case CrossNsErrorCode::kScatterTimeout:
        case CrossNsErrorCode::kIndexCorrupt:           return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status CrossNsStatus(CrossNsErrorCode code, const std::string& detail) {
    const char* cx = CrossNsErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(CrossNsErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::query
