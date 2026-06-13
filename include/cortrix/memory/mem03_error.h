#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::memory::transparency {

/// The 5 MEM03 Memory-Transparency error identities (MEM03 §4.3.4.bis — the
/// invalidate-tool error set the DELETE / invalidate path returns, registered in
/// ARCH §4.1.11.X). Each maps to a stable `CX_ERR_*` string + a GEN-Agent category
/// + retryability + retry_after_ms + the structured_data keys its body MUST carry,
/// via the canonical registry below.
///
/// F-FREEZE-1 template A (C_R1_BRIEFING §3, mirroring memory/mem02_error.h): the D1
/// detail design wrote `Result<T, MemoryTransparencyError>` (double-template) in
/// places, which the project forbids — Cortrix uses `Result<T>` (StatusOr) + `Status`
/// only. A domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code. So
/// Mem03ErrorCode is the *enum of identities*, and MakeMem03Error() turns one (plus
/// optional structured_data) into that boundary error.
///
/// Scope note: MEM03 §7 also lists a "CRUD generic" 5-code set (NOT_FOUND /
/// FORBIDDEN / INVALID_TYPE / CONTENT_TOO_LONG / EDIT_CONCURRENT) used by the
/// GET/POST/PATCH paths. §4.3.4.bis is explicit that the canonical Phase-1 v1.0
/// error family — the one registered in ARCH §4.1.11 + surfaced by P12 v1.0.4
/// `cortrix_memory_invalidate` — is *this* 5-code invalidate set, and the briefing
/// §3 / §2 pin MEM03 to exactly these 5 (MEMORY_NOT_FOUND 404 / USER_MISMATCH 403 /
/// ALREADY_INVALIDATED 410 / INVALIDATE_FAILED 500 / QUOTA 429). The CRUD-path
/// semantics (type validation, content length, optimistic-lock conflict,
/// cross-user 404 mask) are folded onto these 5 where they overlap and otherwise
/// reported via plain InvalidArgument Status (see MemoryTransparency); the §7 CRUD
/// strings are a Phase-2 superset (MEM03 §4.3.4.bis "evolution"). This header is the
/// SoT for the registered 5.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class Mem03ErrorCode {
    kMemoryNotFound,        ///< 404 CX_ERR_MEM03_MEMORY_NOT_FOUND — permanent, not retryable (incl. cross-user mask)
    kUserMismatch,          ///< 403 CX_ERR_MEM03_USER_MISMATCH — auth, not retryable (caller ≠ owner)
    kAlreadyInvalidated,    ///< 410 CX_ERR_MEM03_ALREADY_INVALIDATED — permanent, not retryable (idempotent repeat)
    kInvalidateFailed,      ///< 500 CX_ERR_MEM03_INVALIDATE_FAILED — transient, retryable (DB / oplog / trace write)
    kQuota,                 ///< 429 CX_ERR_MEM03_QUOTA — quota, retryable (per-user/per-ns rate limit)
};

/// Total number of MEM03 error codes (MEM03 §4.3.4.bis = 5). Compile-time anchor
/// for the API-compatibility regression test (the set must not shrink).
constexpr int kMem03ErrorCodeCount = 5;

/// Canonical, immutable attributes of one error code (MEM03 §4.3.4.bis columns).
struct Mem03ErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" string
    int http_status;                          ///< §4.3.4.bis HTTP column (404/403/410/500/429)
    agent_friendly::ErrorCategory category;   ///< permanent/auth/transient/quota
    bool retryable;                           ///< §4.3.4.bis retryable column
    std::optional<int> retry_after_ms;        ///< §4.3.4.bis retry hint (5000 / 60000 / null)
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 5 rows.
const Mem03ErrorInfo& GetMem03ErrorInfo(Mem03ErrorCode code);

/// The "CX_ERR_*" string for `code`.
const char* Mem03ErrorCodeString(Mem03ErrorCode code);

/// The §4.3.4.bis HTTP status code for `code`.
int Mem03ErrorHttpStatus(Mem03ErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5,
/// MEM03 §4.3.4.bis structured_data column). SoT for the Agent-friendly contract;
/// lets call sites + tests verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(Mem03ErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(Mem03ErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeMem03Error(
    Mem03ErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Mem03ErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeMem03Error() re-inflation at the API boundary.
///
/// 410-Gone (ALREADY_INVALIDATED) and 429 (QUOTA) have no dedicated StatusCode in
/// common/status.h; they fold onto the closest coarse code (kAlreadyExists /
/// kUnavailable) — same lossy-but-recoverable pattern as MEM02 budget→kPermissionDenied.
StatusCode Mem03ErrorToStatusCode(Mem03ErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as Mem02Status /
/// RagFusionStatus). This is what `Result<T>` failure paths carry.
Status Mem03Status(Mem03ErrorCode code, const std::string& detail = "");

}  // namespace cortrix::memory::transparency
