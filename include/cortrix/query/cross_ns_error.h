#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::query {

/// The 6 Cross-NS query error identities. Each maps to a stable
/// `CX_ERR_*` string + a GEN-Agent category + retryability + the structured_data
/// keys its body MUST carry, via the canonical registry below.
///
/// 🌟 The cross-NS error responses are the **project-level Agent-friendly reference**
/// (topic 2.7 drives GEN-Agent's first principle — the Agent-friendly contract). All 7 principles must
/// hold: machine-readable code (#1) + enumerable category (#4) + structured_data
/// (#5) + machine-readable retry (#6) + stable/versioned set (#7).
///
/// Per the coding conventions / F-FREEZE-1, Cortrix uses Result<T>+Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code.
/// So CrossNsErrorCode is the *enum of identities*, and MakeCrossNsError() turns
/// one (plus optional structured_data) into that boundary error — mirroring the
/// CatalogErrorCode template.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
///
/// Anti-enumeration (topic 2.6 /): NS-not-found and NS-unauthorized BOTH return
/// kNsUnauthorized — the existence of a namespace is never leaked. There is no
/// separate "NS not found" code on purpose.
enum class CrossNsErrorCode {
    kAuthInvalidCredentials,  ///< 401 — unauthenticated (empty user_id)
    kNsUnauthorized,          ///< 403 — unauthorized / not-found (anti-enumeration)
    kTooManyNamespaces,       ///< 400 — over the per-query NS hard cap (topic 1.5) / plan cap
    kNsTimeout,               ///< 200 + meta.failed — a single NS exceeded ns_query_timeout_ms
    kIndexCorrupt,            ///< 200 + meta.failed — a single NS internal error
    kScatterTimeout,          ///< 200 + meta + partial — overall scatter exceeded total_timeout_ms
};

/// Total number of Cross-NS error codes (= 6). Compile-time anchor for
/// the API-compatibility regression test (the set must not shrink).
constexpr int kCrossNsErrorCodeCount = 6;

/// Canonical, immutable attributes of one error code.
struct CrossNsErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    agent_friendly::ErrorCategory category;   ///< auth/quota/transient/permanent/timeout
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null unless retryable per
    int http_status;                          ///< HTTP column (200 for partial-failure codes)
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 6 rows.
const CrossNsErrorInfo& GetCrossNsErrorInfo(CrossNsErrorCode code);

/// The "CX_ERR_*" string for `code` (convenience over GetCrossNsErrorInfo).
const char* CrossNsErrorCodeString(CrossNsErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5).
/// Empty for codes whose body is contextual. SoT for the Agent-friendly contract;
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(CrossNsErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(CrossNsErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeCrossNsError(
    CrossNsErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Convenience overload for kNsUnauthorized: packs the unauthorized NS list into
/// structured_data.unauthorized_namespaces (topic 2.1 Step 4 / GEN-Agent #5). The
/// list is what the Agent uses to re-query only authorized namespaces.
agent_friendly::AgentFriendlyError MakeNsUnauthorizedError(
    const std::vector<std::string>& unauthorized_namespaces);

/// CrossNsErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeCrossNsError() re-inflation at the API boundary.
StatusCode CrossNsErrorToStatusCode(CrossNsErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as CatalogStatus).
Status CrossNsStatus(CrossNsErrorCode code, const std::string& detail = "");

/// Throwing form — wraps the full Agent-friendly error. AuthorizeNamespaces and
/// other "abort the whole request" paths throw this; the handler catches it
/// and serializes GetError() to the error body.
class CrossNsException : public agent_friendly::AgentFriendlyException {
public:
    explicit CrossNsException(CrossNsErrorCode code,
                              nlohmann::json structured_data = nlohmann::json::object(),
                              const std::string& message = "")
        : agent_friendly::AgentFriendlyException(
              MakeCrossNsError(code, std::move(structured_data), message)),
          code_(code) {}

    CrossNsErrorCode code() const { return code_; }

private:
    CrossNsErrorCode code_;
};

}  // namespace cortrix::query
