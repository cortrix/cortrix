#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::import {

/// The 6 F16a DB-import error identities. Each maps to a stable
/// `CX_ERR_F16A_*` string + a GEN-Agent category + retryability + the HTTP status
/// its body carries, via the canonical registry below.
///
/// Per CODING_CONVENTIONS §3 / F-FREEZE-1, Cortrix uses Result<T>+Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// So F16aErrorCode is the *enum of identities*, and MakeF16aError() turns one
/// (plus optional structured_data) into that boundary error — mirroring the F04
/// CrossNsErrorCode / F12 CatalogErrorCode template A.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
///
/// Note: task_cancelled is NOT an error code (F16a §5.4 note) — cancel is a legal
/// operation that resolves to status="cancelled", carrying no error body.
enum class F16aErrorCode {
    kConnectionFailed,    ///< 503 transient — PostgreSQL connection failed (network / DNS / SSL)
    kAuthDenied,          ///< 403 auth      — per-NS Admin permission insufficient
    kInvalidSql,          ///< 400 permanent — SQL whitelist rejected (write op / denied keyword / bad DSL)
    kTimeout,             ///< 504 timeout   — query exceeded the 5-minute limit
    kRowsLimitExceeded,   ///< 413 quota     — rows over the 10M cap
    kCrossTenantRef,      ///< 403 auth      — cross-tenant connection_ref rejected
    // [R2-M5] Appended (GEN-Agent #7 allows new codes; the existing 6 stay unchanged).
    // Catch-all for an unexpected throw from the import stack (DB driver / allocation),
    // so the handler returns a structured 500 rather than the generic global fallback.
    kInternal,            ///< 500 transient — unexpected internal import failure
};

/// Total number of F16a error codes. Compile-time anchor for the API-compatibility
/// regression test (the set must not shrink; new codes may be appended — GEN-Agent #7).
/// Was 6; +kInternal (R2-M5) = 7.
constexpr int kF16aErrorCodeCount = 7;

/// Canonical, immutable attributes of one error code (F16a §5.4 columns).
struct F16aErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_F16A_*" string
    agent_friendly::ErrorCategory category;   ///< auth/quota/transient/permanent/timeout
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable
    int http_status;                          ///< §5.4 HTTP column
};

/// Look up the canonical attributes for `code`. Total over the enum (never throws
/// / never returns a partial). Single source of truth for the 6 rows.
const F16aErrorInfo& GetF16aErrorInfo(F16aErrorCode code);

/// The "CX_ERR_F16A_*" string for `code` (convenience over GetF16aErrorInfo).
const char* F16aErrorCodeString(F16aErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5).
/// Empty for codes whose body is contextual. SoT for the Agent-friendly contract;
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(F16aErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(F16aErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeF16aError(
    F16aErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// F16aErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeF16aError() re-inflation at the API boundary.
StatusCode F16aErrorToStatusCode(F16aErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_F16A_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as CrossNsStatus).
Status F16aStatus(F16aErrorCode code, const std::string& detail = "");

/// Throwing form — wraps the full Agent-friendly error. Paths that must abort the
/// whole request (a query that fails the security gate, a cross-tenant ref) throw
/// this; the handler catches it and serializes GetError() to the §5.3 error body.
class F16aException : public agent_friendly::AgentFriendlyException {
public:
    explicit F16aException(F16aErrorCode code,
                           nlohmann::json structured_data = nlohmann::json::object(),
                           const std::string& message = "")
        : agent_friendly::AgentFriendlyException(
              MakeF16aError(code, std::move(structured_data), message)),
          code_(code) {}

    F16aErrorCode code() const { return code_; }

private:
    F16aErrorCode code_;
};

}  // namespace cortrix::import
