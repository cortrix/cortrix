#pragma once
#include <string>

#include "cortrix/common/status.h"

namespace cortrix::store {

/// Stable CX_ERR_* identity codes for the Write Coordinator / PWL scope
/// (design, registered under ARCH `CX_ERR_PWL_*`). Per
/// the coding conventions the project uses one error model — `Result<T>` +
/// `Status` — and distinguishes domain errors by a `CX_ERR_*` code string
/// rather than a typed `Result<T, E>`. We carry that code as a stable prefix on
/// the Status message (`"<CODE>: <detail>"`); the API/SDK/MCP boundary lifts it
/// into the AgentFriendlyError `code` field.
namespace pwl_errors {

inline constexpr char kWriteFailed[]            = "CX_ERR_PWL_WRITE_FAILED";
inline constexpr char kCorrupted[]              = "CX_ERR_PWL_CORRUPTED";
inline constexpr char kTxnNotFound[]            = "CX_ERR_PWL_TXN_NOT_FOUND";
inline constexpr char kTxnAlreadyCommitted[]    = "CX_ERR_PWL_TXN_ALREADY_COMMITTED";
inline constexpr char kTxnAlreadyRollback[]     = "CX_ERR_PWL_TXN_ALREADY_ROLLBACK";
inline constexpr char kRollbackCallbackFailed[] = "CX_ERR_PWL_ROLLBACK_CALLBACK_FAILED";
inline constexpr char kRecoveryFailed[]         = "CX_ERR_PWL_RECOVERY_FAILED";
inline constexpr char kDocWriteInProgress[]     = "CX_ERR_PWL_DOC_WRITE_IN_PROGRESS";

}  // namespace pwl_errors

/// Build a Status whose message is `"<code>: <detail>"`, so the stable CX_ERR_*
/// identity survives alongside the human-readable detail. `sc` is the coarse
/// StatusCode the boundary maps to HTTP; the precise identity is `code`.
inline Status PwlStatus(StatusCode sc, const char* code, const std::string& detail) {
    return Status(sc, std::string(code) + ": " + detail);
}

}  // namespace cortrix::store
