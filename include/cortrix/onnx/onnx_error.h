#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::onnx {

/// The 3 ONNX-layer error identities. Each maps to a stable
/// `CX_ERR_*` string + a GEN-Agent category + retryability via the canonical
/// registry below.
///
/// Per CODING_CONVENTIONS §3, Cortrix uses Result<T> + Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// So OnnxErrorCode is the *enum of identities*, and MakeOnnxError() turns one
/// (plus optional structured_data) into that boundary error. This mirrors the
/// catalog::CatalogErrorCode template exactly — the spec's
/// `Result<void, OnnxError>` / standalone `OnnxError` struct (§9.1) is the
/// pre-convention draft; this is its convention-compliant form.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
enum class OnnxErrorCode {
    // --- Startup-time (not retryable; user must intervene) ---
    kRuntimeVersionMismatch,  ///< loaded runtime major != CMake-compiled major
    kOpsetIncompatible,       ///< a registered model's opset is out of range
    // --- Run-time (transient; one retry then surface) ---
    kInferenceFailed,         ///< OrtSession::Run() returned non-OK
};

/// Total number of ONNX error codes (= 3). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kOnnxErrorCodeCount = 3;

/// Canonical, immutable attributes of one error code.
struct OnnxErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    agent_friendly::ErrorCategory category;   ///< permanent / transient / ...
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per §8.3
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 3 rows.
const OnnxErrorInfo& GetOnnxErrorInfo(OnnxErrorCode code);

/// The "CX_ERR_*" string for `code` (convenience over GetOnnxErrorInfo).
const char* OnnxErrorCodeString(OnnxErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry
/// (structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5);
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(OnnxErrorCode code);

/// True iff `structured_data` contains every required key for `code`. Used to
/// assert Agent-friendly completeness before returning an error body.
bool HasRequiredStructuredData(OnnxErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching
/// `structured_data` (the §8.3 required keys are the caller's responsibility at
/// each call site) and an optional human-readable `message`. category /
/// retryable / retry_after_ms are filled from the canonical registry — call
/// sites never restate them. The returned structured_data is always a JSON
/// OBJECT (Agent consumes directly, no JSON.parse).
agent_friendly::AgentFriendlyError MakeOnnxError(
    OnnxErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge an ONNX error to a plain Status for the Result<T>/Status interface
/// surface (F-FREEZE-1). The StatusCode is the coarse mapping of `code`; the
/// message is prefixed with the CX_ERR_* token ("CX_ERR_X: detail") so the exact
/// ONNX identity is recoverable at the API/SDK boundary, which re-inflates the
/// full Agent-friendly body (category / retryable / structured_data) via
/// MakeOnnxError(). Mirrors catalog::CatalogStatus — we deliberately do NOT
/// widen cortrix::Status itself.
Status OnnxStatus(OnnxErrorCode code, const std::string& detail = "");

/// Coarse OnnxErrorCode → StatusCode mapping used by OnnxStatus (exposed for
/// tests / boundary code that needs the mapping without a message).
StatusCode OnnxErrorToStatusCode(OnnxErrorCode code);

}  // namespace cortrix::onnx
