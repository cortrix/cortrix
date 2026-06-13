#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::memory::immunity {

/// The 7 MEM04 Memory-Immunity error identities (MEM04 §5.4, registered in
/// ARCH §4.1.11 — "M-05 +5 + V14 J5 +2 = 7", issue D6). Each maps to a stable
/// `CX_ERR_*` string + a GEN-Agent category + retryability + retry_after_ms + the
/// structured_data keys its body MUST carry, via the canonical registry below.
///
/// F-FREEZE-1 template A (C_R2_BRIEFING §2, mirroring memory/mem03_error.h): the D1
/// detail design wrote `Result<T, ...>` (double-template) in places, which the
/// project forbids — Cortrix uses `Result<T>` (StatusOr) + `Status` only. A domain
/// error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code. So
/// Mem04ErrorCode is the *enum of identities*, and MakeMem04Error() turns one (plus
/// optional structured_data) into that boundary error.
///
/// SoT note: the canonical attributes (http / category / retryable / retry_after_ms /
/// structured_data keys) follow ARCH §4.1.11 (the registered authority), which MEM04
/// §5.4 explicitly defers to (MEM04-rev-1 registers them in ARCH §4.1.11). Where the
/// detail design §5.4 table and ARCH §4.1.11 differ on a structured_data key (ALREADY_OPTED_OUT →
/// `opted_out_at`; REVOKE_DENIED → `required_role`; OPT_OUT_DISABLED → `config_source`),
/// ARCH wins.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class Mem04ErrorCode {
    kSessionNotFound,    ///< 404 CX_ERR_MEM04_SESSION_NOT_FOUND — permanent, not retryable
    kAlreadyOptedOut,    ///< 409 CX_ERR_MEM04_ALREADY_OPTED_OUT — permanent, not retryable (idempotent repeat)
    kNotOptedOut,        ///< 409 CX_ERR_MEM04_NOT_OPTED_OUT — permanent, not retryable (revoke on an active session)
    kRevokeDenied,       ///< 403 CX_ERR_MEM04_REVOKE_DENIED — auth, not retryable (revoke needs admin)
    kOptOutDisabled,     ///< 503 CX_ERR_MEM04_OPT_OUT_DISABLED — permanent, not retryable (mem04.enabled = false)
    kInvalidSessionId,   ///< 422 CX_ERR_MEM04_INVALID_SESSION_ID — permanent, not retryable (not ULID/UUID)
    kMetadataTooLarge,   ///< 422 CX_ERR_MEM04_METADATA_TOO_LARGE — permanent, not retryable (metadata JSON > 4KB)
};

/// Total number of MEM04 error codes (ARCH §4.1.11 = 7). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kMem04ErrorCodeCount = 7;

/// Canonical, immutable attributes of one error code (ARCH §4.1.11 columns).
struct Mem04ErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    int http_status;                          ///< ARCH §4.1.11 HTTP column (404/409/403/503/422)
    agent_friendly::ErrorCategory category;   ///< permanent / auth (all 7 are non-retryable)
    bool retryable;                           ///< ARCH §4.1.11 retryable column (all false)
    std::optional<int> retry_after_ms;        ///< ARCH §4.1.11 retry hint (all null)
};

/// Look up the canonical attributes for `code`. Total over the enum (never throws /
/// never returns a partial). Single source of truth for the 7 rows.
const Mem04ErrorInfo& GetMem04ErrorInfo(Mem04ErrorCode code);

/// The "CX_ERR_*" string for `code`.
const char* Mem04ErrorCodeString(Mem04ErrorCode code);

/// The ARCH §4.1.11 HTTP status code for `code`.
int Mem04ErrorHttpStatus(Mem04ErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5,
/// ARCH §4.1.11 structured_data column). SoT for the Agent-friendly contract; lets
/// call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(Mem04ErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(Mem04ErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeMem04Error(
    Mem04ErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Mem04ErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeMem04Error() re-inflation at the API boundary.
///
/// 409-Conflict (ALREADY_OPTED_OUT / NOT_OPTED_OUT), 503 (OPT_OUT_DISABLED) and 422
/// (INVALID_SESSION_ID / METADATA_TOO_LARGE) have no dedicated StatusCode in
/// common/status.h; they fold onto the closest coarse code (kAlreadyExists /
/// kUnavailable / kInvalidArgument) — same lossy-but-recoverable pattern as
/// Mem03ErrorToStatusCode.
StatusCode Mem04ErrorToStatusCode(Mem04ErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as Mem03Status).
/// This is what `Result<T>` failure paths carry.
Status Mem04Status(Mem04ErrorCode code, const std::string& detail = "");

}  // namespace cortrix::memory::immunity
