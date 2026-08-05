#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::reranker {

/// The 5 reranker error identities. Each maps to a stable `CX_ERR_*`
/// string + a GEN-Agent category + retryability via the canonical registry below.
///
/// Per CODING_CONVENTIONS §3, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// So RerankerErrorCode is the *enum of identities*, and MakeRerankerError() turns
/// one (plus optional structured_data) into that boundary error. This mirrors the
/// F12 catalog::CatalogErrorCode template exactly.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
///
/// NOTE (§5.2 note): single-task ONNX failure / extreme-input score=0 are
/// degradation fallbacks (transient semantics, no client-facing error code) —
/// observed via the cortrix_reranker_failed_tasks_total metric, not this enum.
enum class RerankerErrorCode {
    kRerankerInitFailed,   ///< model/tokenizer/session load failed → fail-fast at startup
    kConfigMismatch,       ///< spc.chunk_size > reranker.max_seq_length → fail-fast at startup
    kRerankerTaskTimeout,  ///< a single rerank task exceeded task_timeout_ms
    kChunkNotFound,        ///< ChunkStore reverse-lookup miss
    kRerankerCircuitOpen,  ///< circuit breaker open → reranker skipped (RRF fallback)
};

/// Total number of reranker error codes (F02 §5.2 = 5). Compile-time anchor for
/// the API-compatibility regression test (the set must not shrink).
constexpr int kRerankerErrorCodeCount = 5;

/// Canonical, immutable attributes of one error code (F02 §5.2 columns).
struct RerankerErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    agent_friendly::ErrorCategory category;   ///< permanent / timeout / transient
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per §5.2
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 5 rows.
const RerankerErrorInfo& GetRerankerErrorInfo(RerankerErrorCode code);

/// The "CX_ERR_*" string for `code` (convenience over GetRerankerErrorInfo).
const char* RerankerErrorCodeString(RerankerErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (F02 §5.2
/// structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5);
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(RerankerErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(RerankerErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// (the §5.2 required keys are the caller's responsibility at each call site) and
/// an optional human-readable `message`. category / retryable / retry_after_ms are
/// filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeRerankerError(
    RerankerErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge a reranker error to a plain Status for the Result<T>/Status surface
/// (F-FREEZE-1). The StatusCode is the coarse mapping of `code`; the message is
/// prefixed with the CX_ERR_* token ("CX_ERR_X: detail") so the exact reranker
/// identity is recoverable at the API/SDK boundary (which re-inflates the full
/// Agent-friendly body via MakeRerankerError). We deliberately do NOT widen
/// cortrix::Status itself.
Status RerankerStatus(RerankerErrorCode code, const std::string& detail = "");

/// Coarse RerankerErrorCode → StatusCode mapping used by RerankerStatus (exposed
/// for tests / boundary code that needs the mapping without a message).
StatusCode RerankerErrorToStatusCode(RerankerErrorCode code);

}  // namespace cortrix::reranker
