#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::metadata {

/// The 3 Metadata Block error identities. 2 errors + 1 warning, each
/// mapping to a stable `CX_ERR_*` / `CX_WARN_*` string + a GEN-Agent category +
/// retryability + the HTTP status its body carries, via the canonical registry below.
///
/// Per the coding conventions / F-FREEZE-1, Cortrix uses Result<T>+Status only (no
/// Result<T,E>); a domain error is carried as the Agent-friendly boundary type
/// cortrix::agent_friendly::AgentFriendlyError, identified by its CX_ERR_* code. So
/// MetadataErrorCode is the *enum of identities*, and MakeMetadataError() turns one
/// (plus optional structured_data) into that boundary error — mirroring the
/// CrossNsErrorCode / RagFusionErrorCode / import error template.
///
/// V1.0 versioning promise (GEN-Agent #7): this set is not removed / renamed /
/// re-categorized; new codes may be appended (api_version stays "v1").
enum class MetadataErrorCode {
    kGenFailed,       ///< CX_ERR_METADATA_GEN_FAILED — block_text generation failed (doc_metadata
                      ///< entirely empty / parse fully failed). permanent, 5xx, not retryable.
    kPartial,         ///< CX_WARN_METADATA_PARTIAL — some fields missing (e.g. page_count unknown)
                      ///< but the Block is still generated (HTTP 200 + meta.warnings[]). warning, not error.
    kFieldImmutable,  ///< CX_ERR_METADATA_FIELD_IMMUTABLE — V1.0 PATCH /metadata attempts
                      ///< to change a field (B' lock, HTTP 405). permanent, not retryable.
};

/// Total number of metadata error codes (= 3 = 2 err + 1 warn). Compile-time
/// anchor for the API-compatibility regression test (the set must not shrink).
constexpr int kMetadataErrorCodeCount = 3;

/// Canonical, immutable attributes of one error code (detailed design columns).
struct MetadataErrorInfo {
    const char* cx_code;                      ///< stable "CX_ERR_*" / "CX_WARN_*" string
    agent_friendly::ErrorCategory category;   ///< warning/permanent
    bool retryable;
    std::optional<int> retry_after_ms;        ///< null for all 3 (none retryable)
    int http_status;                          ///< HTTP column (5xx / 200 / 405)
};

/// Look up the canonical attributes for `code`. Total over the enum (never throws /
/// never returns a partial). Single source of truth for the 3 rows.
const MetadataErrorInfo& GetMetadataErrorInfo(MetadataErrorCode code);

/// The "CX_ERR_*" / "CX_WARN_*" string for `code`.
const char* MetadataErrorCodeString(MetadataErrorCode code);

/// The structured_data keys a `code`'s error body MUST carry (GEN-Agent #5,
/// JSON examples). SoT for the Agent-friendly contract; lets call sites + tests
/// verify the body is complete.
const std::vector<std::string>& RequiredStructuredDataKeys(MetadataErrorCode code);

/// True iff `structured_data` contains every required key for `code`.
bool HasRequiredStructuredData(MetadataErrorCode code,
                               const nlohmann::json& structured_data);

/// Build the Agent-friendly boundary error for `code`, attaching `structured_data`
/// and an optional human-readable `message`. category / retryable / retry_after_ms
/// are filled from the canonical registry — call sites never restate them.
agent_friendly::AgentFriendlyError MakeMetadataError(
    MetadataErrorCode code,
    nlohmann::json structured_data = nlohmann::json::object(),
    const std::string& message = "");

/// MetadataErrorCode → StatusCode coarse mapping (for the Result<T>/Status surface,
/// F-FREEZE-1). Rich category/retryable are preserved via the CX_ERR_* token +
/// MakeMetadataError() re-inflation at the API boundary.
StatusCode MetadataErrorToStatusCode(MetadataErrorCode code);

/// Bridge to a plain Status; message prefixed "CX_ERR_X: detail" so the exact
/// identity is recoverable at the API/SDK boundary (same pattern as ImportStatus /
/// RagFusionStatus). This is what `Result<T>` failure paths carry.
Status MetadataStatus(MetadataErrorCode code, const std::string& detail = "");

}  // namespace cortrix::metadata
