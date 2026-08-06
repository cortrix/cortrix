#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::spc {

/// The 7 data-cleaning error identities. Each maps to a
/// stable `CX_ERR_F10_*` string + a GEN-Agent category + retryability via the
/// canonical registry below — same pattern as catalog::CatalogErrorCode
/// (the template) and spc::ParserErrorCode.
///
/// Per CODING_CONVENTIONS §3, a domain error is carried as the Agent-friendly
/// boundary type cortrix::agent_friendly::AgentFriendlyError, identified by its
/// CX_ERR_* code; MakeCleaningError() builds that boundary error.
enum class CleaningErrorCode : uint8_t {
    kDedupVectorInvalid = 0,     // block.embedding empty/NaN
    kDedupThresholdRange = 1,    // threshold config < 0 or > 1
    kAnomalyConfigInvalid = 2,   // max_chunk_chars ≤ 0
    kPluginTimeout = 3,          // cleaning plugin > plugin_timeout_ms
    kPluginException = 4,        // Plugin threw a C++ exception
    kNsConfigMergeFailed = 5,    // ConfigResolver failed
    kInternalError = 6,          // fallback unknown error
};

/// Total number of data-cleaning error codes (= 7). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kCleaningErrorCodeCount = 7;

/// Canonical, immutable attributes of one error code (§5.1 columns). Mirrors
/// catalog::CatalogErrorInfo / spc::ParserErrorInfo so the registries read alike.
struct CleaningErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_F10_*" string
    agent_friendly::ErrorCategory category;   ///< auth/quota/transient/permanent/timeout
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per §5.1
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the §5.1 rows.
const CleaningErrorInfo& GetCleaningErrorInfo(CleaningErrorCode code);

/// The "CX_ERR_F10_*" string for `code`.
const char* CleaningErrorCodeString(CleaningErrorCode code);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them. The
/// CX_ERR_* identity is also injected into structured_data["code"].
agent_friendly::AgentFriendlyError MakeCleaningError(
    CleaningErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge a cleaning error to a plain Status (F-FREEZE-1). The message is
/// prefixed with the CX_ERR_F10_* token so the exact identity is recoverable at
/// the API/SDK boundary (same pattern as catalog::CatalogStatus). We do NOT
/// widen cortrix::Status itself.
Status CleaningStatus(CleaningErrorCode code, const std::string& detail = "");

/// Coarse CleaningErrorCode → StatusCode mapping used by CleaningStatus.
StatusCode CleaningErrorToStatusCode(CleaningErrorCode code);

}  // namespace cortrix::spc
