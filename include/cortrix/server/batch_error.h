#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::server {

/// The 4 batch-level error identities. Each maps to a stable
/// `CX_ERR_BATCH_*` string + a GEN-Agent category + retryability + retry_after_ms
/// + the structured_data keys its body MUST carry, via the canonical registry
/// below. Mirrors the task registry pattern (async/task_error.h): the enum is the
/// set of identities and MakeBatchError() turns one into the Agent-friendly
/// boundary type cortrix::agent_friendly::AgentFriendlyError.
///
/// These are batch *request envelope* faults (the whole request is rejected with
/// a single GEN-Agent error). Per-document failures inside an accepted batch are
/// NOT in this enum — they reuse the originating Feature's existing CX_ERR_* codes
/// in meta.failed[] ("avoid error-code explosion").
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class BatchErrorCode {
    kSizeExceeded,     ///< 400 CX_ERR_BATCH_SIZE_EXCEEDED — documents > 100 (topic 1)
    kPayloadTooLarge,  ///< 413 CX_ERR_BATCH_PAYLOAD_TOO_LARGE — body > 100MB (topic 1)
    kEmpty,            ///< 400 CX_ERR_BATCH_EMPTY — documents is an empty array
    kDuplicateDocId,   ///< 400 CX_ERR_BATCH_DUPLICATE_DOC_ID — duplicate doc_id in one batch (topic 4 Q2)
};

/// Total number of batch-level error codes (= 4 rows).
/// Compile-time anchor for the API-compatibility regression test (the set must
/// not shrink).
constexpr int kBatchErrorCodeCount = 4;

/// Canonical, immutable attributes of one batch error code (§2.4.1 columns).
struct BatchErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_BATCH_*" string
    int http_status;                          ///< HTTP status (400 / 413)
    agent_friendly::ErrorCategory category;   ///< §2.4.1 category column (all permanent)
    bool retryable;                           ///< §2.4.1 retryable column (all false)
    std::optional<int> retry_after_ms;        ///< null for all batch-level codes
};

/// Look up the canonical attributes for `code`. Total over the enum (never
/// throws / never returns a partial). Single source of truth for the 4 rows.
const BatchErrorInfo& GetBatchErrorInfo(BatchErrorCode code);

/// The "CX_ERR_BATCH_*" string for `code`.
const char* BatchErrorCodeString(BatchErrorCode code);

/// The HTTP status code for `code`.
int BatchErrorHttpStatus(BatchErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5).
/// SoT for the Agent-friendly contract; lets call sites + tests verify the body
/// is complete.
const std::vector<std::string>& BatchRequiredStructuredDataKeys(BatchErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool BatchHasRequiredStructuredData(BatchErrorCode code,
                                    const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeBatchError(
    BatchErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

}  // namespace cortrix::server
