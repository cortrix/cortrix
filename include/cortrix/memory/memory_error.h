#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::memory::transparency {

/// The 5 memory-transparency error identities (the
/// invalidate-tool error set the DELETE / invalidate path returns, registered in
/// ARCH). Each maps to a stable `CX_ERR_*` string + a GEN-Agent category
/// + retryability + retry_after_ms + the structured_data keys its body MUST carry,
/// via the canonical registry below.
///
/// F-FREEZE-1 template A (mirroring memory/memory_extract_error.h): the
/// detail design wrote `Result<T, MemoryTransparencyError>` (double-template) in
/// places, which the project forbids — Cortrix uses `Result<T>` (StatusOr) + `Status`
/// only. A domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code. So
/// MemoryErrorCode is the *enum of identities*, and MakeMemoryError() turns one (plus
/// optional structured_data) into that boundary error.
///
/// Scope note: the design also lists a "CRUD generic" 5-code set (NOT_FOUND /
/// FORBIDDEN / INVALID_TYPE / CONTENT_TOO_LONG / EDIT_CONCURRENT) used by the
/// GET/POST/PATCH paths. is explicit that the canonical Phase-1 v1.0
/// error family — the one in the server error registry, surfaced by the MCP server
/// `cortrix_memory_invalidate` — is *this* 5-code invalidate set, and the briefing
/// which pins transparency to exactly these 5 (MEMORY_NOT_FOUND 404 / USER_MISMATCH 403 /
/// ALREADY_INVALIDATED 410 / INVALIDATE_FAILED 500 / QUOTA 429). The CRUD-path
/// semantics (type validation, content length, optimistic-lock conflict,
/// cross-user 404 mask) are folded onto these 5 where they overlap and otherwise
/// reported via plain InvalidArgument Status (see MemoryTransparency); the CRUD
/// strings are a Phase-2 superset. This header is the
/// SoT for the registered 5.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class MemoryErrorCode {
    kMemoryNotFound,        ///< 404 CX_ERR_MEMORY_NOT_FOUND — permanent, not retryable (incl. cross-user mask)
    kUserMismatch,          ///< 403 CX_ERR_MEMORY_USER_MISMATCH — auth, not retryable (caller ≠ owner)
    kAlreadyInvalidated,    ///< 410 CX_ERR_MEMORY_ALREADY_INVALIDATED — permanent, not retryable (idempotent repeat)
    kInvalidateFailed,      ///< 500 CX_ERR_MEMORY_INVALIDATE_FAILED — transient, retryable (DB / oplog / trace write)
    kQuota,                 ///< 429 CX_ERR_MEMORY_QUOTA — quota, retryable (per-user/per-ns rate limit)
};

/// Total number of memory-transparency error codes (= 5). Compile-time anchor
/// for the API-compatibility regression test (the set must not shrink).
constexpr int kMemoryErrorCodeCount = 5;

/// Canonical, immutable attributes of one error code.
struct MemoryErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    int http_status;                          ///< HTTP column (404/403/410/500/429)
    agent_friendly::ErrorCategory category;   ///< permanent/auth/transient/quota
    bool retryable;                           ///< retryable column
    std::optional<int> retry_after_ms;        ///< retry hint (5000 / 60000 / null)
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 5 rows.
const MemoryErrorInfo& GetMemoryErrorInfo(MemoryErrorCode code);

/// The "CX_ERR_*" string for `code`.
const char* MemoryErrorCodeString(MemoryErrorCode code);

/// The HTTP status code for `code`.
int MemoryErrorHttpStatus(MemoryErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5,
/// structured_data column). SoT for the Agent-friendly contract;
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(MemoryErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(MemoryErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeMemoryError(
    MemoryErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// MemoryErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeMemoryError() re-inflation at the API boundary.
///
/// 410-Gone (ALREADY_INVALIDATED) and 429 (QUOTA) have no dedicated StatusCode in
/// common/status.h; they fold onto the closest coarse code (kAlreadyExists /
/// kUnavailable) — same lossy-but-recoverable pattern as the extraction budget→kPermissionDenied.
StatusCode MemoryErrorToStatusCode(MemoryErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as MemoryExtractStatus /
/// RagFusionStatus). This is what `Result<T>` failure paths carry.
Status MemoryStatus(MemoryErrorCode code, const std::string& detail = "");

}  // namespace cortrix::memory::transparency
