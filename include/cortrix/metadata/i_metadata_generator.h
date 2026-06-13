#pragma once
#include "cortrix/common/result.h"
#include "cortrix/metadata/metadata_types.h"
#include "cortrix/observability/trace_context.h"

// F08 Metadata Block — generator interface (detailed design §2.1).
namespace cortrix::metadata {

/// Abstract metadata-block generator (detailed design §2.1). Phase 1 V1.0 has one
/// implementation, RuleBasedMetadataGenerator (D4 lock — pure rule extraction, no LLM dependency,
/// always available / L0 fallback). Phase 2 F41 doc-summary takes an independent path (does not implement this interface).
///
/// Generate returns Result<GeneratorOutput> (F-FREEZE-1: Result<T> = StatusOr,
/// single template; the domain error identity travels via MetadataErrorCode /
/// MakeMetadataError at the API boundary — see metadata_error.h). A partial
/// generation (e.g. page_count unknown) still succeeds (HTTP 200) with
/// output.missing_fields populated; only an empty doc_metadata / failed F06 parse
/// produces an error Result (CX_ERR_METADATA_GEN_FAILED, §5.2 degradation-path table).
class IMetadataGenerator {
public:
    virtual ~IMetadataGenerator() = default;

    /// Generate the document-level Metadata Block from `input`. `ctx` is the
    /// OBS_SPEC §5.3 trace context (nullptr when untraced).
    virtual Result<GeneratorOutput> Generate(
        const GeneratorInput& input,
        const observability::TraceContext* ctx = nullptr) = 0;
};

}  // namespace cortrix::metadata
