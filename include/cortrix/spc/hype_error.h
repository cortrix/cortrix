#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::spc {

/// The 6 HyPE error identities (registered in the server error registry). Each
/// maps to a stable `CX_ERR_HYPE_*` string + a GEN-Agent category + retryability
/// via the canonical registry below.
///
/// Per CODING_CONVENTIONS §3, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// HypeErrorCode is the *enum of identities*; MakeHypeError() turns one (plus
/// structured_data) into that boundary error. Mirrors the enricher_error /
/// reranker_error template.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
enum class HypeErrorCode {
    kLlmTimeout,            ///< LLM call exceeded the timeout
    kLlmInvalidOutput,      ///< LLM returned empty / unparseable output
    kLlmBudgetExceeded,     ///< per-Feature / global LLM budget cap hit
    kQuestionParseFailed,   ///< parsed question count != expected K (§6.2)
    kSchemaVersionMismatch, ///< HypeSchemaProvider unexpected version step
    kParentNotFound,        ///< ParentChunkStore.GetParent miss / DB error (§6.3)
};

/// Total HyPE error codes (= 6). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kHypeErrorCodeCount = 6;

/// Canonical, immutable attributes of one error code.
struct HypeErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_HYPE_*" string
    agent_friendly::ErrorCategory category;   ///< timeout/transient/quota/permanent
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per §7
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 6 rows.
const HypeErrorInfo& GetHypeErrorInfo(HypeErrorCode code);

/// The "CX_ERR_HYPE_*" string for `code`.
const char* HypeErrorCodeString(HypeErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry
/// (structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5).
const std::vector<std::string>& RequiredStructuredDataKeys(HypeErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(HypeErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// + an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeHypeError(
    HypeErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Coarse HypeErrorCode → StatusCode mapping (exposed for tests / boundary code).
StatusCode HypeErrorToStatusCode(HypeErrorCode code);

/// Bridge a HyPE error to a plain Status for the Result<T>/Status surface
/// (F-FREEZE-1). The message is prefixed with the CX_ERR_HYPE_* token so the exact
/// identity is recoverable at the API/SDK boundary (which re-inflates the full
/// Agent-friendly body via MakeHypeError). cortrix::Status is not widened.
Status HypeStatus(HypeErrorCode code, const std::string& detail = "");

}  // namespace cortrix::spc
