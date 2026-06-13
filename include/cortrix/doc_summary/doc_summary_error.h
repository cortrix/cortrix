#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::doc_summary {

/// The 7 F41 Document Summary error identities (F41 §7, registered in ARCH
/// §4.1.11). Each maps to a stable `CX_ERR_F41_*` string + a GEN-Agent category +
/// retryability via the canonical registry below.
///
/// Per CODING_CONVENTIONS §3, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// DocSummaryErrorCode is the *enum of identities*; MakeDocSummaryError() turns
/// one (plus structured_data) into that boundary error. Mirrors the F38
/// hype_error / F03 enricher_error template (Briefing §2 template A).
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
enum class DocSummaryErrorCode {
    kLlmTimeout,            ///< LLM call timed out (F42 retry 3x + DLQ)
    kLlmInvalidOutput,      ///< structured JSON output unparseable
    kLlmBudgetExceeded,     ///< LLM budget cap hit
    kDocTooLarge,           ///< document too large to summarize (permanent)
    kSchemaVersionMismatch, ///< F41SchemaProvider unexpected version step
    kFallbackFailed,        ///< in-doc chunk-recall fallback failed (F41-5 G)
    kFts5FallbackFailed,    ///< hybrid FTS5 index build / query failed (F41-8)
};

/// Total F41 error codes (F41 §7 = 7). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kDocSummaryErrorCodeCount = 7;

/// Canonical, immutable attributes of one error code (F41 §7 columns).
struct DocSummaryErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_F41_*" string
    agent_friendly::ErrorCategory category;   ///< timeout/transient/quota/permanent
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per §7
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 7 rows.
const DocSummaryErrorInfo& GetDocSummaryErrorInfo(DocSummaryErrorCode code);

/// The "CX_ERR_F41_*" string for `code`.
const char* DocSummaryErrorCodeString(DocSummaryErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (F41 §7
/// structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5).
const std::vector<std::string>& RequiredStructuredDataKeys(DocSummaryErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(DocSummaryErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// + an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeDocSummaryError(
    DocSummaryErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Coarse DocSummaryErrorCode → StatusCode mapping (exposed for tests / boundary
/// code that needs the mapping without a message).
StatusCode DocSummaryErrorToStatusCode(DocSummaryErrorCode code);

/// Bridge an F41 error to a plain Status for the Result<T>/Status surface
/// (F-FREEZE-1). The message is prefixed with the CX_ERR_F41_* token so the exact
/// identity is recoverable at the API/SDK boundary (which re-inflates the full
/// Agent-friendly body via MakeDocSummaryError). cortrix::Status is not widened.
Status DocSummaryStatus(DocSummaryErrorCode code, const std::string& detail = "");

}  // namespace cortrix::doc_summary
