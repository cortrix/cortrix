#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::agent_trace {

/// The 7 agent-observability error identities. Each maps to a
/// stable `CX_ERR_F13_*` string + a GEN-Agent category + retryability via the
/// canonical registry below.
///
/// Per CODING_CONVENTIONS §3 / F-FREEZE-1, Cortrix uses Result<T> + Status only
/// (no Result<T,E>); a domain error is carried as the Agent-friendly boundary
/// type cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_*
/// code. So F13ErrorCode is the *enum of identities*, and MakeF13Error() turns
/// one (plus optional structured_data) into that boundary error. This mirrors the
/// reranker::RerankerErrorCode template exactly.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
enum class F13ErrorCode {
    kSessionNotFound,     ///< admin cross-user session does not exist (§8.1)
    kInvalidFilter,       ///< invalid header / filter value (topic 9 — renamed from INVALID_SESSION_ID)
    kInteractionNotFound, ///< GET /interactions/{id}/sources — interaction missing (§8.2)
    kUnauthorized,        ///< non-admin cross-user query (§8.1/§8.3 anti-leak)
    kMcpSessionInvalid,   ///< MCP capability sessionId failed validation (v1.0.2)
    kSessionExpired,      ///< session past the retention window (v1.0.2)
    kInternal,            ///< unexpected server fault
};

/// Total number of observability error codes (= 7). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kF13ErrorCodeCount = 7;

/// Canonical, immutable attributes of one error code.
struct F13ErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_F13_*" string
    agent_friendly::ErrorCategory category;   ///< auth / permanent / transient
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null for every code in this family
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 7 rows.
const F13ErrorInfo& GetF13ErrorInfo(F13ErrorCode code);

/// The "CX_ERR_F13_*" string for `code` (convenience over GetF13ErrorInfo).
const char* F13ErrorCodeString(F13ErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (
/// structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5);
/// lets call sites + tests verify the body is complete. value_preview on
/// INVALID_FILTER is optional (PII guard) so it is NOT in the required set.
const std::vector<std::string>& RequiredStructuredDataKeys(F13ErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(F13ErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// (the §9.2 required keys are the caller's responsibility at each call site) and
/// an optional human-readable `message`. category / retryable / retry_after_ms are
/// filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeF13Error(
    F13ErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge an observability error to a plain Status for the Result<T>/Status surface
/// (F-FREEZE-1). The StatusCode is the coarse mapping of `code`; the message is
/// prefixed with the CX_ERR_F13_* token ("CX_ERR_F13_X: detail") so the exact
/// identity is recoverable at the API/SDK boundary (which re-inflates the full
/// Agent-friendly body via MakeF13Error). We deliberately do NOT widen
/// cortrix::Status itself.
Status F13Status(F13ErrorCode code, const std::string& detail = "");

/// Coarse F13ErrorCode → StatusCode mapping used by F13Status (exposed for tests
/// / boundary code that needs the mapping without a message).
StatusCode F13ErrorToStatusCode(F13ErrorCode code);

}  // namespace cortrix::agent_trace
