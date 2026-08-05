#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::memory {

/// The 5 LLM memory-extraction error identities. Each maps to a
/// stable `CX_ERR_*` string + a GEN-Agent category + retryability + retry_after_ms
/// + the structured_data keys its body MUST carry, via the canonical registry below.
///
/// The detail design wrote `Result<T,
/// XxxError>` (double-template) in places, which F-FREEZE-1 forbids — Cortrix uses
/// `Result<T>` (StatusOr) + `Status` only. A domain error is carried as the
/// Agent-friendly boundary type cortrix::agent_friendly::AgentFriendlyError,
/// identified by its CX_ERR_* code. So Mem02ErrorCode is the *enum of identities*,
/// and MakeMem02Error() turns one (plus optional structured_data) into that
/// boundary error — mirroring query/rag_fusion_error.h + async/f42_error.h (template A).
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class Mem02ErrorCode {
    kExtractLlmTimeout,     ///< 504 CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT — transient/timeout, retryable
    kExtractInvalidOutput,  ///< 500 CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT — transient (LLM bad JSON), retryable
    kExtractBudgetExceeded, ///< 429 CX_ERR_MEM02_EXTRACT_BUDGET_EXCEEDED — quota, not retryable
    kContradictionAmbiguous,///< 500 CX_ERR_MEM02_CONTRADICTION_AMBIGUOUS — transient (low-confidence judge), retryable
    kLlmDisabled,           ///< 503 CX_ERR_MEM02_LLM_DISABLED — permanent (NullEnricher mode), not retryable
};

/// Total number of memory-extraction error codes (= 5). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kMem02ErrorCodeCount = 5;

/// Canonical, immutable attributes of one error code.
struct Mem02ErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    int http_status;                          ///< §5.3 HTTP column (504/500/429/503)
    agent_friendly::ErrorCategory category;   ///< transient/timeout/quota/permanent
    bool retryable;                           ///< §5.3 retryable column
    std::optional<int> retry_after_ms;        ///< §5.3 retry hint (5000 for retryable LLM, null otherwise)
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 5 rows.
const Mem02ErrorInfo& GetMem02ErrorInfo(Mem02ErrorCode code);

/// The "CX_ERR_*" string for `code`.
const char* Mem02ErrorCodeString(Mem02ErrorCode code);

/// The §5.3 HTTP status code for `code`.
int Mem02ErrorHttpStatus(Mem02ErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5,
/// structured_data example). SoT for the Agent-friendly contract; lets
/// call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(Mem02ErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(Mem02ErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeMem02Error(
    Mem02ErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Mem02ErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeMem02Error() re-inflation at the API boundary.
StatusCode Mem02ErrorToStatusCode(Mem02ErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as
/// RagFusionStatus / F42Status). This is what `Result<T>` failure paths carry.
Status Mem02Status(Mem02ErrorCode code, const std::string& detail = "");

}  // namespace cortrix::memory
