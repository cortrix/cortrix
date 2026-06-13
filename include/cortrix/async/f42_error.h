#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::async {

/// The 11 F42 Document-Async error identities (F42 §6.2). Each maps to a stable
/// `CX_ERR_*` string + a GEN-Agent category + retryability + retry_after_ms + the
/// structured_data keys its body MUST carry, via the canonical registry below.
///
/// F42 §3 (B_R2_BRIEFING template A): the D1 detail design wrote `Result<T,
/// XxxError>` (double-template) in places, which F-FREEZE-1 forbids — Cortrix
/// uses `Result<T>` (StatusOr) + `Status` only. A domain error is carried as the
/// Agent-friendly boundary type cortrix::agent_friendly::AgentFriendlyError,
/// identified by its CX_ERR_* code. So F42ErrorCode is the *enum of identities*,
/// and MakeF42Error() turns one (plus optional structured_data) into that
/// boundary error — mirroring query/rag_fusion_error.h (template A) / query/cross_ns_error.h.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class F42ErrorCode {
    kInvalidRequest,             ///< 400 CX_ERR_INVALID_REQUEST — permanent, not retryable
    kMaxPagesExceeded,           ///< 403 CX_ERR_MAX_PAGES_EXCEEDED — permanent (CE >200 / > Ent limit)
    kTaskNotFound,               ///< 404 CX_ERR_TASK_NOT_FOUND — permanent
    kTaskTimeout,                ///< 408 CX_ERR_TASK_TIMEOUT — transient/timeout, 30 min timeout
    kDocProcessingInProgress,    ///< 409 CX_ERR_DOC_PROCESSING_IN_PROGRESS — transient (preventive, post topic 2.1)
    kTaskCancelling,             ///< 423 CX_ERR_TASK_CANCELLING — permanent (repeat cancel while cancelling)
    kParseFailed,                ///< 500 CX_ERR_PARSE_FAILED — permanent
    kStorageFailed,              ///< 500 CX_ERR_STORAGE_FAILED — permanent (disk full / temp file lost)
    kWorkerBusy,                 ///< 500 CX_ERR_WORKER_BUSY — transient
    kZombieTaskCleanup,          ///< 500 CX_ERR_ZOMBIE_TASK_CLEANUP — permanent (restart-cleanup of crashed task)
    kServiceUnavailable,         ///< 503 CX_ERR_SERVICE_UNAVAILABLE — transient
};

/// Total number of F42 error codes (F42 §6.2 = 11 HTTP error rows; the 202
/// TASK_SUBMITTED success row is not an error code). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kF42ErrorCodeCount = 11;

/// Canonical, immutable attributes of one error code (F42 §6.2 columns).
struct F42ErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    int http_status;                          ///< §6.2 HTTP column (400/403/404/408/409/423/500/503)
    agent_friendly::ErrorCategory category;   ///< permanent/transient/timeout
    bool retryable;                           ///< §6.2 retryable column
    std::optional<int> retry_after_ms;        ///< §6.2 retry_after_ms column (null / 10000 / 30000 / 60000)
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 11 rows.
const F42ErrorInfo& GetF42ErrorInfo(F42ErrorCode code);

/// The "CX_ERR_*" string for `code`.
const char* F42ErrorCodeString(F42ErrorCode code);

/// The §6.2 HTTP status code for `code`.
int F42ErrorHttpStatus(F42ErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5,
/// F42 §6.2 structured_data column). SoT for the Agent-friendly contract; lets
/// call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(F42ErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(F42ErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeF42Error(
    F42ErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// F42ErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeF42Error() re-inflation at the API boundary.
StatusCode F42ErrorToStatusCode(F42ErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as
/// RagFusionStatus / CrossNsStatus). This is what `Result<T>` failure paths carry.
Status F42Status(F42ErrorCode code, const std::string& detail = "");

}  // namespace cortrix::async
