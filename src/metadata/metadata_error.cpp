#include "cortrix/metadata/metadata_error.h"

#include <utility>

namespace cortrix::metadata {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code (detailed design §5.1). Defined as function-local statics so each
// returns a stable reference. The switch in GetMetadataErrorInfo is intentionally
// exhaustive: building with -Wall -Wextra (-Wswitch) turns "added a code without a
// row" into a warning (treated as a build failure), so the registry can't silently
// drift from the enum.
//
// §5.1: all 3 codes are non-retryable (retry_after_ms = null). GEN_FAILED is a 5xx
// permanent error (F06 produced no usable metadata — re-uploading won't help until
// the source is fixed); PARTIAL is a warning rendered into the HTTP 200 success body
// (meta.warnings[], §5.3); FIELD_IMMUTABLE is the 405 V1.0 returns for PATCH /metadata
// (D9 B' lock, Phase 2 F08 Block Versioning changes it to 200).
constexpr MetadataErrorInfo kGenFailed
    {"CX_ERR_METADATA_GEN_FAILED",       ErrorCategory::kPermanent, false, std::nullopt, 500};
constexpr MetadataErrorInfo kPartial
    {"CX_WARN_METADATA_PARTIAL",         ErrorCategory::kPermanent, false, std::nullopt, 200};
constexpr MetadataErrorInfo kFieldImmutable
    {"CX_ERR_METADATA_FIELD_IMMUTABLE",  ErrorCategory::kPermanent, false, std::nullopt, 405};

}  // namespace

const MetadataErrorInfo& GetMetadataErrorInfo(MetadataErrorCode code) {
    switch (code) {
        case MetadataErrorCode::kGenFailed:      return kGenFailed;
        case MetadataErrorCode::kPartial:        return kPartial;
        case MetadataErrorCode::kFieldImmutable: return kFieldImmutable;
    }
    // Unreachable for a valid enum value; defensive fallback keeps the function
    // total (and avoids a -Wreturn-type warning).
    return kGenFailed;
}

const char* MetadataErrorCodeString(MetadataErrorCode code) {
    return GetMetadataErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(MetadataErrorCode code) {
    // §5.1.1 example bodies — the structured_data keys the Agent needs to act.
    // Function-local statics → stable refs.
    static const std::vector<std::string> kEmpty{};
    // GEN_FAILED (§5.1.1 example A): the Agent needs to know which stage failed +
    // what is missing + the F06 parse status + what downstream was blocked.
    static const std::vector<std::string> kGenFailedKeys{
        "doc_id", "namespace_id", "stage", "missing_fields",
        "f06_parse_status", "downstream_blocked"};
    // FIELD_IMMUTABLE (§5.1.1 example B): which fields were attempted + the full
    // immutable set + the Phase 2 feature + the re-upload workaround.
    static const std::vector<std::string> kFieldImmutableKeys{
        "doc_id", "namespace_id", "attempted_fields",
        "v1_immutable_fields", "phase2_feature", "workaround"};
    // PARTIAL: the §5.3 warning body carries missing_fields + fallback_reason inside
    // the meta.warnings[] item (built by the response layer), so the error-registry
    // contract for the warning code itself is contextual (no required top-level keys).
    static const std::vector<std::string> kPartialKeys{};

    switch (code) {
        case MetadataErrorCode::kGenFailed:      return kGenFailedKeys;
        case MetadataErrorCode::kPartial:        return kPartialKeys;
        case MetadataErrorCode::kFieldImmutable: return kFieldImmutableKeys;
    }
    return kEmpty;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(MetadataErrorCode code,
                               const nlohmann::json& structured_data) {
    const std::vector<std::string>& keys = RequiredStructuredDataKeys(code);
    if (!structured_data.is_object()) {
        return keys.empty();
    }
    for (const std::string& key : keys) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeMetadataError(MetadataErrorCode code,
                                     nlohmann::json structured_data,
                                     const std::string& message) {
    const MetadataErrorInfo& info = GetMetadataErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode MetadataErrorToStatusCode(MetadataErrorCode code) {
    switch (code) {
        // generation failed (no usable F06 output) → kInternal (5xx).
        case MetadataErrorCode::kGenFailed:      return StatusCode::kInternal;
        // partial is a warning, not a hard failure; the coarse Status side maps it to
        // kInvalidArgument only if a caller forces it onto the error surface (the
        // normal path returns Ok + missing_fields, not this Status).
        case MetadataErrorCode::kPartial:        return StatusCode::kInvalidArgument;
        // immutable-field PATCH → kPermissionDenied (closest coarse code for a 405;
        // rich category/HTTP preserved via the CX_ERR_ token + boundary MakeMetadataError).
        case MetadataErrorCode::kFieldImmutable: return StatusCode::kPermissionDenied;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status MetadataStatus(MetadataErrorCode code, const std::string& detail) {
    const char* cx = MetadataErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(MetadataErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::metadata
