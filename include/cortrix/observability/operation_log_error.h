#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::observability {

/// The 6 operation-log error identities. Each maps to a stable
/// `CX_ERR_OPLOG_*` string + a GEN-Agent category + retryability via the canonical
/// registry below.
///
/// Mirrors the CatalogErrorCode pattern (enum-of-identities + table-driven
/// registry + MakeOplogError → AgentFriendlyError) per the coding conventions:
/// Cortrix uses Result<T> + Status only (no Result<T,E>); a domain error is
/// carried as the Agent-friendly boundary type cortrix::agent_friendly::
/// AgentFriendlyError, identified by its CX_ERR_* code. So OplogErrorCode is the
/// *enum of identities*, and MakeOplogError() turns one (plus its required
/// structured_data) into that boundary error.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1"). Note the
/// "note": CX_ERR_OPLOG_QUOTA_EXCEEDED is deliberately NOT a member — the
/// write-path auto-cleanup absorbs the row cap, so clients never retry on quota.
enum class OplogErrorCode {
    kInvalidFilter,           // CX_ERR_OPLOG_INVALID_FILTER
    kInvalidTimestampRange,   // CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE
    kPaginationOutOfRange,    // CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE
    kUnauthorized,            // CX_ERR_OPLOG_UNAUTHORIZED
    kCleanupRunning,          // CX_ERR_OPLOG_CLEANUP_RUNNING
    kInternal,                // CX_ERR_OPLOG_INTERNAL
};

/// Total number of operation-log error codes (= 6). Compile-time anchor
/// for the API-compatibility regression test (the set must not shrink).
constexpr int kOplogErrorCodeCount = 6;

/// CX_ERR_OPLOG_CLEANUP_RUNNING base backoff ("5000 + jitter"). The base is
/// canonical here; the caller adds jitter when it builds the structured_data
/// `retry_at_ms` so retries from many clients don't thunder at the same instant.
constexpr int kOplogCleanupRetryBaseMs = 5000;

/// Canonical, immutable attributes of one error code.
struct OplogErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_OPLOG_*" string
    agent_friendly::ErrorCategory category;   ///< auth/quota/transient/permanent/timeout
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 6 rows.
const OplogErrorInfo& GetOplogErrorInfo(OplogErrorCode code);

/// The "CX_ERR_OPLOG_*" string for `code` (convenience over GetOplogErrorInfo).
const char* OplogErrorCodeString(OplogErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry
/// ("structured_data required" column). This is the SoT for the Agent-friendly
/// contract (GEN-Agent #5) and lets call sites + tests verify the body is
/// complete before it is returned.
const std::vector<std::string>& RequiredStructuredDataKeys(OplogErrorCode code);

/// True iff `structured_data` contains every required key for `code`. Used to
/// assert Agent-friendly completeness before returning an error body.
bool HasRequiredStructuredData(OplogErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// (the required keys are the caller's responsibility at each call site) and
/// an optional human-readable `message`. category / retryable / retry_after_ms are
/// filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeOplogError(
    OplogErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Bridge an oplog error to a plain Status for the Result<T>/Status interface
/// surface (F-FREEZE-1). The StatusCode is the coarse mapping of `code`; the
/// message is prefixed with the CX_ERR_OPLOG_* token ("CX_ERR_OPLOG_X: detail")
/// so the exact oplog identity is recoverable at the API/SDK boundary, which
/// re-inflates the full Agent-friendly body (category / retryable /
/// structured_data) via MakeOplogError(). Follows the CatalogStatus precedent —
/// we deliberately do NOT widen cortrix::Status itself.
Status OplogStatus(OplogErrorCode code, const std::string& detail = "");

/// Coarse OplogErrorCode → StatusCode mapping used by OplogStatus (exposed for
/// tests / boundary code that needs the mapping without a message).
StatusCode OplogErrorToStatusCode(OplogErrorCode code);

}  // namespace cortrix::observability
