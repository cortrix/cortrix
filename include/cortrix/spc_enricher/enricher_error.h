#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::spc {

/// The 6 enricher error identities (F03 §1.6 / §5.1). Each maps to a stable
/// `CX_ERR_ENRICHER_*` string + a GEN-Agent category + retryability via the
/// canonical registry below.
///
/// Per CODING_CONVENTIONS §3 / B-R1 §3 (F-FREEZE-1 reconcile), Cortrix uses
/// Result<T> + Status only (no Result<T,E>); a domain error is carried as the
/// Agent-friendly boundary type cortrix::agent_friendly::AgentFriendlyError,
/// identified by its CX_ERR_* code. So EnricherErrorCode is the *enum of
/// identities*, and MakeEnricherError() turns one (plus optional
/// structured_data) into that boundary error. Mirrors reranker_error.h /
/// catalog::CatalogErrorCode exactly.
///
/// The design's §2.5b "EnricherCategory" enum is reconciled to the frozen
/// agent_friendly::ErrorCategory (auth/quota/transient/permanent/timeout) — no
/// parallel enum (same discipline as spc::parser.h reusing ErrorCategory).
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
enum class EnricherErrorCode {
    kInitFailed,   ///< api_key missing / endpoint unreachable → fallback NullEnricher (startup)
    kLlmTimeout,   ///< a single LLM task exceeded task_timeout_ms
    kLlmApi,       ///< LLM HTTP 5xx / network error
    kParse,        ///< response JSON parse failure (L1 field / L2 chunk / L3 whole batch)
    kBudget,       ///< budget_cap_usd exceeded → all-subsequent score=0 + fallback NullEnricher
    kRateLimit,    ///< LLM 429 rate limit (Retry-After backoff)
};

/// Total number of enricher error codes (F03 §1.6 = 6). Compile-time anchor for
/// the API-compatibility regression test (the set must not shrink).
constexpr int kEnricherErrorCodeCount = 6;

/// Canonical, immutable attributes of one error code (F03 §5.1 columns).
struct EnricherErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_ENRICHER_*" string
    agent_friendly::ErrorCategory category;   ///< auth/quota/transient/permanent/timeout
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null (== -1 in matrix) unless retryable per §5.1
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 6 rows.
const EnricherErrorInfo& GetEnricherErrorInfo(EnricherErrorCode code);

/// The "CX_ERR_ENRICHER_*" string for `code` (convenience over GetEnricherErrorInfo).
const char* EnricherErrorCodeString(EnricherErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (F03 §5.1
/// structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5);
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(EnricherErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(EnricherErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// (the §5.1 required keys are the caller's responsibility at each call site) and
/// an optional human-readable `message`. category / retryable / retry_after_ms are
/// filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeEnricherError(
    EnricherErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge an enricher error to a plain Status for the Result<T>/Status surface
/// (F-FREEZE-1). The StatusCode is the coarse mapping of `code`; the message is
/// prefixed with the CX_ERR_ENRICHER_* token ("CX_ERR_X: detail") so the exact
/// enricher identity is recoverable at the API/SDK boundary (which re-inflates
/// the full Agent-friendly body via MakeEnricherError). We deliberately do NOT
/// widen cortrix::Status itself.
Status EnricherStatus(EnricherErrorCode code, const std::string& detail = "");

/// Coarse EnricherErrorCode → StatusCode mapping used by EnricherStatus (exposed
/// for tests / boundary code that needs the mapping without a message).
StatusCode EnricherErrorToStatusCode(EnricherErrorCode code);

}  // namespace cortrix::spc
