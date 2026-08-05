#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::retrieval {

/// The 5 BGE-M3-sparse error identities (registered in the server error registry).
/// Each maps to a stable `CX_ERR_F40_*` string + a GEN-Agent category +
/// retryability via the canonical registry below.
///
/// Per CODING_CONVENTIONS §3, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// SparseErrorCode is the *enum of identities*; MakeSparseError() turns one
/// (plus structured_data) into that boundary error. Mirrors the
/// reranker_error template exactly (Briefing §3 template A).
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
enum class SparseErrorCode {
    kInferenceFailed,        ///< BGE-M3 ONNX inference failed (dense+sparse both)
    kSparseSerializeFailed,  ///< sparse_vec serialization failed (e.g. term_id out of range)
    kInvertedIndexWriteFailed,  ///< SPLADE inverted index write failed (concurrent / IO)
    kSparseRetrieverFailed,  ///< query-time sparse retriever fault → L2 fallback
    kOnnxRuntimeInitFailed,  ///< startup BGE-M3 model load failed
};

/// Total sparse-retrieval error codes (= 5). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kSparseErrorCodeCount = 5;

/// Canonical, immutable attributes of one error code.
struct SparseErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_F40_*" string
    agent_friendly::ErrorCategory category;   ///< transient / permanent
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per §8
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 5 rows.
const SparseErrorInfo& GetSparseErrorInfo(SparseErrorCode code);

/// The "CX_ERR_F40_*" string for `code`.
const char* SparseErrorCodeString(SparseErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (
/// structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5).
const std::vector<std::string>& RequiredStructuredDataKeys(SparseErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(SparseErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// + an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeSparseError(
    SparseErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Coarse SparseErrorCode → StatusCode mapping (exposed for tests / boundary
/// code that needs the mapping without a message).
StatusCode SparseErrorToStatusCode(SparseErrorCode code);

/// Bridge a sparse-retrieval error to a plain Status for the Result<T>/Status surface
/// (F-FREEZE-1). The message is prefixed with the CX_ERR_F40_* token so the
/// exact identity is recoverable at the API/SDK boundary (which re-inflates the
/// full Agent-friendly body via MakeSparseError). cortrix::Status itself is not
/// widened.
Status SparseStatus(SparseErrorCode code, const std::string& detail = "");

}  // namespace cortrix::retrieval
