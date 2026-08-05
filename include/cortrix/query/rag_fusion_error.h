#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::query {

/// The 6 RAG-Fusion error identities. 5 errors + 1 warning, each mapping
/// to a stable `CX_ERR_*` / `CX_WARN_*` string + a GEN-Agent category +
/// retryability + the structured_data keys its body MUST carry, via the canonical
/// registry below.
///
/// The detail design wrote `Result<T,
/// RagFusionError>` (double-template), which F-FREEZE-1 forbids — Cortrix uses
/// `Result<T>` (StatusOr) + `Status` only. A domain error is carried as the
/// Agent-friendly boundary type cortrix::agent_friendly::AgentFriendlyError,
/// identified by its CX_ERR_* code. So RagFusionErrorCode is the *enum of
/// identities*, and MakeRagFusionError() turns one (plus optional structured_data)
/// into that boundary error — mirroring the CrossNsErrorCode /
/// CatalogErrorCode template (query/cross_ns_error.h).
///
/// topic 6 commercial sensitivity: CX_WARN_RAG_FUSION_DEGRADED is C-class — it ONLY
/// surfaces when a user has explicitly enabled RAG-Fusion (ns_config.enabled=true) and an
/// LLM call then fails; users who never enable it never see it (the LLM is never
/// called). All other codes are B-class (internal, only via ?explain=true).
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class RagFusionErrorCode {
    kDegraded,           ///< CX_WARN_RAG_FUSION_DEGRADED — C-class warning (topic 4),
                         ///< variant generation failed → single-query fallback
    kLlmTimeout,         ///< CX_ERR_RAG_FUSION_LLM_TIMEOUT — B-class, LLM call timed out
    kLlmQuota,           ///< CX_ERR_RAG_FUSION_LLM_QUOTA — B-class, LLM quota exhausted
    kLlmCircuitOpen,     ///< CX_ERR_RAG_FUSION_LLM_CIRCUIT_OPEN — B-class, breaker open
    kInvalidResponse,    ///< CX_ERR_RAG_FUSION_INVALID_RESPONSE — B-class, LLM output
                         ///< failed JSON-schema validation (permanent, not retryable)
    kConfigInvalid,      ///< CX_ERR_RAG_FUSION_CONFIG_INVALID — startup error (not in
                         ///< query response), bad rag_fusion_config field
};

/// Total number of RAG-Fusion error codes (= 6 = 5 err + 1 warn).
/// Compile-time anchor for the API-compatibility regression test (the set must
/// not shrink).
constexpr int kRagFusionErrorCodeCount = 6;

/// Canonical, immutable attributes of one error code.
struct RagFusionErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" / "CX_WARN_*" string
    agent_friendly::ErrorCategory category;   ///< transient/timeout/quota/permanent
    bool retryable;
    std::optional<int> retry_after_ms;        ///< per §7 (5000 / 60000 / 30000 / null)
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 6 rows.
const RagFusionErrorInfo& GetRagFusionErrorInfo(RagFusionErrorCode code);

/// The "CX_ERR_*" / "CX_WARN_*" string for `code`.
const char* RagFusionErrorCodeString(RagFusionErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5,
/// structured_data column). SoT for the Agent-friendly contract; lets call
/// sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(RagFusionErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(RagFusionErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeRagFusionError(
    RagFusionErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// RagFusionErrorCode → StatusCode coarse mapping (for the Result<T>/Status
/// surface, F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_*
/// token + MakeRagFusionError() re-inflation at the API boundary.
StatusCode RagFusionErrorToStatusCode(RagFusionErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as
/// CrossNsStatus / CatalogStatus). This is what `Result<T>` failure paths carry.
Status RagFusionStatus(RagFusionErrorCode code, const std::string& detail = "");

}  // namespace cortrix::query
