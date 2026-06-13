#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::scoring {

/// The 2 F07 Semantic Score error identities (F07 §4.4). F07 is an internal module
/// (few error scenarios, §4.4 note), so the set is small: an out-of-range level (defensive
/// bottom-out) and a bad α config. Each maps to a stable `CX_ERR_F07_*` string + a
/// GEN-Agent category + retryability + the structured_data keys its body MUST carry,
/// via the canonical registry below.
///
/// Per CODING_CONVENTIONS §3 / F-FREEZE-1, Cortrix uses Result<T>+Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code. So
/// F07ErrorCode is the *enum of identities*, and MakeScoringError() turns one (plus
/// optional structured_data) into that boundary error — mirroring the F16a / F36 /
/// F08 template A.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class F07ErrorCode {
    kLevelInvalid,   ///< CX_ERR_F07_LEVEL_INVALID — ComputeLevel output > 4 or < 0
                     ///< (exceptional-path bottom-out). permanent, not retryable. structured_data:
                     ///< {level_received, max_allowed, scoring_input}.
    kConfigInvalid,  ///< CX_ERR_F07_CONFIG_INVALID — α config outside [0, 1] (startup
                     ///< error). permanent, not retryable. structured_data:
                     ///< {alpha_received, valid_range, config_source}.
};

/// Total number of F07 error codes (F07 §4.4 = 2). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kF07ErrorCodeCount = 2;

/// Canonical, immutable attributes of one error code (F07 §4.4 columns).
struct F07ErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_F07_*" string
    agent_friendly::ErrorCategory category;   ///< permanent (both)
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null for both (neither retryable, §4.4)
};

/// Look up the canonical attributes for `code`. Total over the enum (never throws /
/// never returns a partial). Single source of truth for the 2 rows.
const F07ErrorInfo& GetF07ErrorInfo(F07ErrorCode code);

/// The "CX_ERR_F07_*" string for `code`.
const char* F07ErrorCodeString(F07ErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5, §4.4
/// structured_data column). SoT for the Agent-friendly contract; lets call sites +
/// tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(F07ErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(F07ErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeScoringError(
    F07ErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// F07ErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeScoringError() re-inflation at the API boundary.
StatusCode F07ErrorToStatusCode(F07ErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_F07_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as F16aStatus).
/// This is what `Result<T>` failure paths carry.
Status ScoringStatus(F07ErrorCode code, const std::string& detail = "");

}  // namespace cortrix::scoring
