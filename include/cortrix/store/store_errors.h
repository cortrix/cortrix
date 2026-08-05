#pragma once
#include <string>

#include "cortrix/common/status.h"

// Store-layer Repository-pattern error codes.
//
// The Store-layer interfaces (ParentChunkStore today; future ChunkStore impls)
// share one error model. Per CODING_CONVENTIONS § 3 the project uses Result<T> /
// Status and distinguishes domain errors by a stable CX_ERR_* code string rather
// than a typed Result<T, E>; we carry that code as a prefix on the Status message
// ("<CODE>: <detail>"). The design's `Result<ParentChunk, StoreError>` /
// `StoreError{code,...}` (§ 2.5/§ 2.5.1, written pre-F-FREEZE-1) reconciles to
// this here. Same shape as store/pwl_errors.h.
namespace cortrix::store {

namespace store_errors {

inline constexpr char kNotFound[] = "CX_ERR_STORE_NOT_FOUND";  ///< key absent (e.g. parent_id) — permanent
inline constexpr char kDbError[]  = "CX_ERR_STORE_DB_ERROR";   ///< underlying SQLite/DB error — transient (retryable)

}  // namespace store_errors

/// Build a Status whose message is "<code>: <detail>", preserving the stable
/// CX_ERR_STORE_* identity alongside the human-readable detail. `sc` is the coarse
/// StatusCode the boundary maps to HTTP; the precise identity is `code`. The
/// API/SDK/MCP boundary lifts `code` into the AgentFriendlyError `code` field
/// (NOT_FOUND → permanent; DB_ERROR → transient/retryable, § 2.5.1).
inline Status StoreStatus(StatusCode sc, const char* code, const std::string& detail) {
    return Status(sc, std::string(code) + ": " + detail);
}

}  // namespace cortrix::store
