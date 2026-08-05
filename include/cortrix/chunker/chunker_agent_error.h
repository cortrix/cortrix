#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

// Chunker Agent-friendly error registry. Mirrors
// reranker/reranker_error.h (template A): an enum of error identities + a canonical
// registry (category / retryable / required structured_data keys) + MakeChunkerError
// turning one into the GEN-Agent boundary body. Per CODING_CONVENTIONS § 3 the
// engine surface uses Result<T>/Status (chunker_errors.h ChunkStatus carries the
// CX_ERR_CHUNK_* token); this header is the API/SDK/MCP boundary inflation.
//
// The 3 HARD errors (§ 5.1 rows 1-3) map to ErrorCategory; the 2 WARNINGS (§ 5.1
// rows 4-5, "category: warning") are non-blocking and surface into meta.warnings
// (§ 5.3), NOT the error body — built via MakeChunkWarning() below since the
// frozen ErrorCategory enum has no `warning` member.
namespace cortrix::chunker {

/// The 3 hard chunker error identities (§ 5.1, blocking). The 2 warnings
/// (CX_WARN_CHUNK_*) are not in this enum — they are meta.warnings entries.
/// V1.0 versioning promise (GEN-Agent #7): never removed / renamed / re-categorized;
/// new codes may only be appended.
enum class ChunkerErrorCode {
    kSizeInvalid,    ///< child_size > reranker.max_seq_length (startup) — permanent
    kEmptyDocument,  ///< parsed doc produced no usable chunk — permanent
    kOverflow,       ///< parent count > cap AND flat fallback failed — permanent
};

constexpr int kChunkerErrorCodeCount = 3;

/// Canonical, immutable attributes of one error code (§ 5.1 columns).
struct ChunkerErrorInfo {
    const char* cx_code;
    agent_friendly::ErrorCategory category;
    bool retryable;
    std::optional<int> retry_after_ms;
};

const ChunkerErrorInfo& GetChunkerErrorInfo(ChunkerErrorCode code);
const char* ChunkerErrorCodeString(ChunkerErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (§ 5.1
/// structured_data column). SoT for the GEN-Agent #5 contract.
const std::vector<std::string>& RequiredStructuredDataKeys(ChunkerErrorCode code);
bool HasRequiredStructuredData(ChunkerErrorCode code, const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code` (category / retryable /
/// retry_after_ms from the registry; call sites supply structured_data + message).
agent_friendly::AgentFriendlyError MakeChunkerError(
    ChunkerErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// Coarse ChunkerErrorCode → StatusCode (all 3 are caller-side permanent →
/// kInvalidArgument; exposed for tests / boundary code).
StatusCode ChunkerErrorToStatusCode(ChunkerErrorCode code);

/// Bridge to a plain Status (CX token prefixed on the message), for the
/// Result<T>/Status engine surface. Mirrors RerankerStatus.
Status ChunkerErrorStatus(ChunkerErrorCode code, const std::string& detail = "");

// --- Non-blocking warnings (§ 5.1 rows 4-5 / § 5.3 meta.warnings) ---------------

/// The 2 chunker warning codes. Non-blocking; emitted into meta.warnings, never
/// the error body.
enum class ChunkerWarningCode {
    kTruncated,  ///< runtime chunk_size > max_seq_length (reranker truncated)
    kFallback,   ///< flat fallback triggered (parent count > threshold)
};

const char* ChunkerWarningCodeString(ChunkerWarningCode code);

/// Build one meta.warnings[] JSON object: {code, message, structured_data}. The
/// boundary appends these to meta.warnings (§ 5.3). category is implicitly
/// "warning" (not an ErrorCategory value).
nlohmann::json MakeChunkWarning(
    ChunkerWarningCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

}  // namespace cortrix::chunker
