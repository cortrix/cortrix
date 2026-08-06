#pragma once
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::retrieval {

/// Thrown by an ICragClassifierBackend when a single inference fails transiently
/// (L3 path). CragEvaluator catches it, retries N times with
/// exponential back-off, then transparently degrades to the all-Correct verdict
/// (CX_ERR_CRAG_INFERENCE_FAILED). Backends must NOT let any other exception escape
/// across the IClassifier boundary.
class CragInferenceError : public std::runtime_error {
public:
    explicit CragInferenceError(const std::string& what) : std::runtime_error(what) {}
};

/// The 4 CRAG-evaluation error identities. Each maps to a stable
/// `CX_ERR_CRAG_*` string + a GEN-Agent category + retryability via the canonical
/// registry below.
///
/// Per the coding conventions, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// So CragErrorCode is the *enum of identities*, and MakeCragError() turns one
/// (plus optional structured_data) into that boundary error. This mirrors the
/// reranker::RerankerErrorCode template (range-A) exactly.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
enum class CragErrorCode {
    kClassifierLoadFailed,  ///< DistilBERT-tiny ONNX load failed → fail-fast at startup (L2)
    kInferenceFailed,       ///< single-query classifier inference failed → retry then degrade (L3)
    kThresholdInvalid,      ///< NS crag_config threshold field out of [0,1] / mis-ordered
    kFallbackTriggered,     ///< transparent degrade to all-Correct fired (retries exhausted)
};

/// Total number of CRAG error codes (= 4). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kCragErrorCodeCount = 4;

/// Canonical, immutable attributes of one error code.
struct CragErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_CRAG_*" string
    agent_friendly::ErrorCategory category;   ///< permanent / transient
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 4 rows.
const CragErrorInfo& GetCragErrorInfo(CragErrorCode code);

/// The "CX_ERR_CRAG_*" string for `code` (convenience over GetCragErrorInfo).
const char* CragErrorCodeString(CragErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry
/// (structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5);
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(CragErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(CragErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// (the required keys are the caller's responsibility at each call site) and
/// an optional human-readable `message`. category / retryable / retry_after_ms are
/// filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeCragError(
    CragErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge a CRAG error to a plain Status for the Result<T>/Status surface
/// (F-FREEZE-1). The StatusCode is the coarse mapping of `code`; the message is
/// prefixed with the CX_ERR_* token ("CX_ERR_X: detail") so the exact CRAG
/// identity is recoverable at the API/SDK boundary (which re-inflates the full
/// Agent-friendly body via MakeCragError). We deliberately do NOT widen
/// cortrix::Status itself.
Status CragStatus(CragErrorCode code, const std::string& detail = "");

/// Coarse CragErrorCode → StatusCode mapping used by CragStatus (exposed for
/// tests / boundary code that needs the mapping without a message).
StatusCode CragErrorToStatusCode(CragErrorCode code);

}  // namespace cortrix::retrieval
