#pragma once
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::query {

/// Thrown by an IComplexityClassifierBackend when a single inference fails
/// transiently (F39 §6.1 / §7.3 L3 path). QueryComplexityClassifier catches it,
/// retries `max_inference_retries` times with exponential back-off (50/100/200ms),
/// then transparently defaults to the Complex path (CX_ERR_F39_INFERENCE_FAILED).
/// Backends must NOT let any other exception escape across the IClassifier
/// boundary (IClassifier::Classify is total per F39 §5.1 / retrieval/classifier.h).
class RouterInferenceError : public std::runtime_error {
public:
    explicit RouterInferenceError(const std::string& what) : std::runtime_error(what) {}
};

/// The 4 query-router error identities. Each maps to a stable
/// `CX_ERR_F39_*` string + a GEN-Agent category + retryability via the canonical
/// registry below.
///
/// Per CODING_CONVENTIONS / B_R1+C_R2 template A, Cortrix uses Result<T> + Status
/// only (no Result<T,E>); a domain error is carried as the Agent-friendly boundary
/// type cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_*
/// code. So RouterErrorCode is the *enum of identities*, and MakeRouterError()
/// turns one (plus optional structured_data) into that boundary error — mirroring
/// the F37 CragErrorCode / F36 RagFusionErrorCode template exactly.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may only be appended.
enum class RouterErrorCode {
    kClassifierLoadFailed,  ///< CX_ERR_F39_CLASSIFIER_LOAD_FAILED — DistilBERT-tiny ONNX load failed (L2, permanent)
    kInferenceFailed,       ///< CX_ERR_F39_INFERENCE_FAILED — single-query inference failed → retry then degrade (L3, transient)
    kForceRouteInvalid,     ///< CX_ERR_F39_FORCE_ROUTE_INVALID — ?route= / NS force_route is not auto/simple/complex/chat (permanent)
    kFallbackTriggered,     ///< CX_ERR_F39_FALLBACK_TRIGGERED — transparent degrade to Complex fired (transient)
};

/// Total number of query-router error codes (F39 §4.3 = 4). Compile-time anchor
/// for the API-compatibility regression test (the set must not shrink).
constexpr int kRouterErrorCodeCount = 4;

/// Canonical, immutable attributes of one error code (F39 §4.3 columns).
struct RouterErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_F39_*" string
    agent_friendly::ErrorCategory category;   ///< permanent / transient
    bool retryable;
    std::optional<int> retry_after_ms;        ///< per §4.3 (100 for inference; null/none otherwise)
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 4 rows.
const RouterErrorInfo& GetRouterErrorInfo(RouterErrorCode code);

/// The "CX_ERR_F39_*" string for `code` (convenience over GetRouterErrorInfo).
const char* RouterErrorCodeString(RouterErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (F39 §4.3
/// structured_data column). SoT for the Agent-friendly contract (GEN-Agent #5);
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(RouterErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(RouterErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// (the §4.3 required keys are the caller's responsibility at each call site) and
/// an optional human-readable `message`. category / retryable / retry_after_ms are
/// filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeRouterError(
    RouterErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge a router error to a plain Status for the Result<T>/Status surface. The
/// StatusCode is the coarse mapping of `code`; the message is prefixed with the
/// CX_ERR_* token ("CX_ERR_X: detail") so the exact identity is recoverable at the
/// API/SDK boundary (which re-inflates the full Agent-friendly body via
/// MakeRouterError). We deliberately do NOT widen cortrix::Status itself.
Status RouterStatus(RouterErrorCode code, const std::string& detail = "");

/// Coarse RouterErrorCode → StatusCode mapping used by RouterStatus (exposed for
/// tests / boundary code that needs the mapping without a message).
StatusCode RouterErrorToStatusCode(RouterErrorCode code);

}  // namespace cortrix::query
