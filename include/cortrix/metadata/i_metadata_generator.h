#pragma once
#include "cortrix/common/result.h"
#include "cortrix/metadata/metadata_types.h"
#include "cortrix/observability/trace_context.h"

// Metadata Block — generator interface.
namespace cortrix::metadata {

/// Abstract metadata-block generator (detailed design). Phase 1 V1.0 has one
/// implementation, RuleBasedMetadataGenerator (lock — pure rule extraction, no LLM dependency,
/// always available / L0 fallback). The doc-summary path is independent (does not implement this interface).
///
/// Generate returns Result<GeneratorOutput> (F-FREEZE-1: Result<T> = StatusOr,
/// single template; the domain error identity travels via MetadataErrorCode /
/// MakeMetadataError at the API boundary — see metadata_error.h). A partial
/// generation (e.g. page_count unknown) still succeeds (HTTP 200) with
/// output.missing_fields populated; only an empty doc_metadata / failed parse
/// produces an error Result (CX_ERR_METADATA_GEN_FAILED, degradation-path table).
class IMetadataGenerator {
public:
    virtual ~IMetadataGenerator() = default;

    /// Generate the document-level Metadata Block from `input`. `ctx` is the
    /// OBS_SPEC trace context (nullptr when untraced).
    virtual Result<GeneratorOutput> Generate(
        const GeneratorInput& input,
        const observability::TraceContext* ctx = nullptr) = 0;
};

}  // namespace cortrix::metadata
