#pragma once
#include <string>

#include "cortrix/common/status.h"

// Chunker error codes. Same one-error-model convention as
// store/pwl_errors.h (the coding conventions): a fallible op returns Result<T> /
// Status, domain identity is a stable CX_* code carried as a prefix on the Status
// message ("<CODE>: <detail>"); the API/SDK/MCP boundary lifts it into the
// AgentFriendlyError `code` field with the retryable/category/structured_data.
namespace cortrix::chunker {

namespace chunk_errors {

// Hard errors (StatusCode-mapped) ------------------------------------------------
inline constexpr char kSizeInvalid[]    = "CX_ERR_CHUNK_SIZE_INVALID";    ///< child_size > reranker.max_seq_length (startup)
inline constexpr char kEmptyDocument[]  = "CX_ERR_CHUNK_EMPTY_DOCUMENT";  ///< parsed doc has no usable page
inline constexpr char kOverflow[]       = "CX_ERR_CHUNK_OVERFLOW";        ///< parent count > cap AND flat fallback failed

// Warnings (non-blocking, surfaced into meta.warnings) ---------------------------
inline constexpr char kWarnTruncated[]  = "CX_WARN_CHUNK_TRUNCATED";      ///< runtime chunk_size > max_seq_length (reranker truncates)
inline constexpr char kWarnFallback[]   = "CX_WARN_CHUNK_FALLBACK";       ///< flat fallback triggered (parent count > threshold)

}  // namespace chunk_errors

/// Build a Status whose message is "<code>: <detail>", preserving the stable
/// CX_ERR_CHUNK_* identity alongside the human-readable detail. `sc` is the coarse
/// StatusCode the boundary maps to HTTP; the precise identity is `code`.
inline Status ChunkStatus(StatusCode sc, const char* code, const std::string& detail) {
    return Status(sc, std::string(code) + ": " + detail);
}

}  // namespace cortrix::chunker
