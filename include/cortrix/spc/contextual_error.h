#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::spc {

/// The 5 contextual-retrieval error identities (registered in the
/// §4.1.11). Each maps to a stable `CX_ERR_F35_*` string + a GEN-Agent category +
/// retryability via the canonical registry below.
///
/// Per CODING_CONVENTIONS §3, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// ContextualErrorCode is the *enum of identities*; MakeContextualError() turns
/// one (plus structured_data) into that boundary error. Mirrors the
/// hype_error / enricher_error template.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
enum class ContextualErrorCode {
    kLlmFailed,        ///< LLM transient failure (timeout / rate limit / network) — §7.3 L3
    kBudgetExceeded,   ///< per-NS soft quota exhausted
    kPromptInjection,  ///< LLM output exceeds max_output_tokens x2 (defense)
    kEmbeddingFailed,  ///< BGE-M3 contextualized embedding failed
    kStartupNoLlm,     ///< LLM unavailable at startup (§7.2 L2)
};

/// Total contextual-retrieval error codes (= 5). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kContextualErrorCodeCount = 5;

/// Canonical, immutable attributes of one error code.
struct ContextualErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_F35_*" string
    agent_friendly::ErrorCategory category;   ///< transient/quota/permanent
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per §8
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 5 rows.
const ContextualErrorInfo& GetContextualErrorInfo(ContextualErrorCode code);

/// The "CX_ERR_F35_*" string for `code`.
const char* ContextualErrorCodeString(ContextualErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry
/// (structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5).
const std::vector<std::string>& RequiredStructuredDataKeys(ContextualErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(ContextualErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// + an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeContextualError(
    ContextualErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Coarse ContextualErrorCode → StatusCode mapping (exposed for tests / boundary
/// code that needs the mapping without a message).
StatusCode ContextualErrorToStatusCode(ContextualErrorCode code);

/// Bridge a contextual-retrieval error to a plain Status for the Result<T>/Status surface
/// (F-FREEZE-1). The message is prefixed with the CX_ERR_F35_* token so the exact
/// identity is recoverable at the API/SDK boundary (which re-inflates the full
/// Agent-friendly body via MakeContextualError). cortrix::Status is not widened.
Status ContextualStatus(ContextualErrorCode code, const std::string& detail = "");

}  // namespace cortrix::spc
