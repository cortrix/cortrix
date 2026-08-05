#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::deploy {

/// The 4 deployment error identities. Each maps to a stable
/// `CX_ERR_*` string + a GEN-Agent category + retryability + retry_after_ms + the
/// structured_data keys its body MUST carry, via the canonical registry below.
///
/// Same convention as catalog_error.h / task_error.h (CODING_CONVENTIONS §3):
/// Cortrix uses Result<T> (StatusOr) + Status only. A domain error is carried as
/// the Agent-friendly boundary type cortrix::agent_friendly::AgentFriendlyError,
/// identified by its CX_ERR_* code. So DeployErrorCode is the *enum of identities*,
/// and MakeDeployError() turns one (plus optional structured_data) into that boundary
/// error — call sites never restate category / retryable / retry_after_ms.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class DeployErrorCode {
    kDiskFull,                  ///< 507 CX_ERR_DISK_FULL — permanent, disk usage >= crit threshold
    kShutdownInProgress,        ///< 503 CX_ERR_SHUTDOWN_IN_PROGRESS — transient (retry after restart)
    kHealthCheckFailed,         ///< 503 CX_ERR_HEALTH_CHECK_FAILED — transient (health endpoint self-error)
    kBfNotReady,                ///< 503 CX_ERR_BF_NOT_READY — transient (retry after Bloom Filter rebuild)
};

/// Total number of deployment error codes (= 4). Compile-time anchor for the
/// API-compatibility regression test (the set must not shrink).
constexpr int kDeployErrorCodeCount = 4;

/// Canonical, immutable attributes of one error code.
struct DeployErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    int http_status;                          ///< HTTP status (507/503)
    agent_friendly::ErrorCategory category;   ///< permanent / transient
    bool retryable;                           ///< §12 retryable column
    std::optional<int> retry_after_ms;        ///< §12 retry_after_ms column (null unless retryable)
};

/// Look up the canonical attributes for `code`. Total over the enum (never throws
/// / never returns a partial). Single source of truth for the 4 rows.
const DeployErrorInfo& GetDeployErrorInfo(DeployErrorCode code);

/// The "CX_ERR_*" string for `code` (convenience over GetDeployErrorInfo).
const char* DeployErrorCodeString(DeployErrorCode code);

/// The HTTP status code for `code`.
int DeployErrorHttpStatus(DeployErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5,
/// structured_data column). SoT for the Agent-friendly contract;
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(DeployErrorCode code);

/// True iff `structured_data` contains every required key for `code`. Used to
/// assert Agent-friendly completeness before returning an error body.
bool HasRequiredStructuredData(DeployErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeDeployError(
    DeployErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// DeployErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeDeployError() re-inflation at the API boundary.
StatusCode DeployErrorToStatusCode(DeployErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as TaskStatus /
/// CatalogStatus). This is what `Result<T>` failure paths carry.
Status DeployStatus(DeployErrorCode code, const std::string& detail = "");

}  // namespace cortrix::deploy
