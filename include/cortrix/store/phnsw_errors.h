#pragma once
#include <string>

#include "cortrix/common/status.h"

namespace cortrix::store {

/// P-HNSW error taxonomy. These name the failure conditions internal
/// to the index; at the public API boundary they are surfaced through
/// cortrix::Status (CODING_CONVENTIONS §3 — single Status error model, with the
/// CX_ERR_PHNSW_* string codes documented in ERROR_CODE_SDK_MAP).
///
/// v1.1 (V5 #9): BLOCK_ID_NOT_FOUND was removed — MarkDelete is idempotent and
/// returns Ok for a missing block_id, so no caller needs that code. Enumerator 7
/// is left as a reserved gap to keep the existing values stable.
enum class PhnswError {
    OK = 0,
    VECTOR_DIM_MISMATCH = 1,
    WAL_WRITE_FAILED = 2,
    WAL_CORRUPTED = 3,
    SNAPSHOT_FAILED = 4,
    SNAPSHOT_CORRUPTED = 5,
    INDEX_FULL = 6,
    // 7: reserved — was BLOCK_ID_NOT_FOUND (removed in v1.1, MarkDelete returns Ok).
    RECOVERY_FAILED = 8,
    WAL_SYNC_FAILED = 9,
};

/// Stable CX_ERR_* identity codes for the P-HNSW scope
/// (registered as `CX_ERR_PHNSW_*`). Per CODING_CONVENTIONS § 3 the
/// project uses one error model — `Result<T>` + `Status` — and distinguishes
/// domain errors by a `CX_ERR_*` code string rather than a typed `Result<T, E>`.
/// The code is carried as a stable prefix on the Status message
/// (`"<CODE>: <detail>"`); the API/SDK/MCP boundary lifts it into the
/// AgentFriendlyError `code` field. Mirrors the `pwl_errors` convention.
namespace phnsw_errors {

inline constexpr char kVectorDimMismatch[] = "CX_ERR_PHNSW_VECTOR_DIM_MISMATCH";
inline constexpr char kWalWriteFailed[]    = "CX_ERR_PHNSW_WAL_WRITE_FAILED";
inline constexpr char kWalCorrupted[]      = "CX_ERR_PHNSW_WAL_CORRUPTED";
inline constexpr char kWalSyncFailed[]     = "CX_ERR_PHNSW_WAL_SYNC_FAILED";
inline constexpr char kSnapshotFailed[]    = "CX_ERR_PHNSW_SNAPSHOT_FAILED";
inline constexpr char kSnapshotCorrupted[] = "CX_ERR_PHNSW_SNAPSHOT_CORRUPTED";
inline constexpr char kIndexFull[]         = "CX_ERR_PHNSW_INDEX_FULL";
inline constexpr char kRecoveryFailed[]    = "CX_ERR_PHNSW_RECOVERY_FAILED";
inline constexpr char kNotReady[]          = "CX_ERR_PHNSW_NOT_READY";

}  // namespace phnsw_errors

/// Build a Status whose message is `"<code>: <detail>"`, so the stable CX_ERR_*
/// identity survives alongside the human-readable detail. `sc` is the coarse
/// StatusCode the boundary maps to HTTP; the precise identity is `code`.
inline Status PhnswStatus(StatusCode sc, const char* code, const std::string& detail) {
    return Status(sc, std::string(code) + ": " + detail);
}

}  // namespace cortrix::store
