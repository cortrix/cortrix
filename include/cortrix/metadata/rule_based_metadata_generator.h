#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/metadata/i_metadata_generator.h"
#include "cortrix/metadata/metadata_types.h"

// Metadata Block — Phase 1 V1.0 rule-based generator.
namespace cortrix::metadata {

/// The sole Phase 1 V1.0 implementation (detailed design / lock): pure rule extraction, no LLM. Maps the
/// consumed DocumentMetadata + the owned FileInfo/ProcessingStats DTOs into
/// (a) block_text — a natural-language sentence over the 7-10 core fields (lock, embedding
/// input), and (b) metadata_json — the full 26-field schema (+ V2 rework). block_id
/// is a fresh ULID (id/ulid.h, produced by the chunker — reuse, do not recreate).
///
/// Standalone: the embedding vector is left empty — embedding(block_text)
/// + P-HNSW insert is integration pipeline wiring. parent_count / child_count come from the
/// caller's ProcessingStats (real source = the chunker). The doc-summary
/// doc_fts5_index product sync is wired in SPCPipeline from the derived columns below,
/// keeping this generator pure and free of SQLite / doc-summary table dependencies.
class RuleBasedMetadataGenerator : public IMetadataGenerator {
public:
    RuleBasedMetadataGenerator() = default;

    /// The detailed design main generation flow. Returns CX_ERR_METADATA_GEN_FAILED when there is no usable
    /// metadata (empty filename AND no source_uri AND failed parse status — i.e. the parser
    /// produced nothing); otherwise Ok, with output.missing_fields listing any
    /// optional fields that could not be filled (→ CX_WARN_METADATA_PARTIAL upstream).
    Result<GeneratorOutput> Generate(
        const GeneratorInput& input,
        const observability::TraceContext* ctx = nullptr) override;

    // --- Exposed building blocks (core functions, detailed design; ≥95% coverage) ---

    /// Assemble block_text (lock: natural-language sentence) from the 7-10 core fields (filename /
    /// mime_type / page_count / upload_time / lang / tags / parser). Not in block_text:
    /// block_id / doc_id / namespace_id / *_version / file_size / counts / business
    /// fields (detailed design). Stable, deterministic ordering so embeddings are reproducible.
    static std::string BuildBlockText(const GeneratorInput& input);

    /// Assemble the full 26-field metadata_json (detailed design). Missing optional fields
    /// are filled per (page_count → null when unknown), and every key in the
    /// schema is present. `missing_fields` is appended with any A-class field
    /// that fell back to null (drives coverage_ratio + CX_WARN_METADATA_PARTIAL).
    static nlohmann::json BuildMetadataJson(const GeneratorInput& input,
                                            std::vector<std::string>* missing_fields);

    /// Derivation rule for doc_fts5_index fields ← metadata fields (V1.0).
    /// Pure mapping, no table dependency: {doc_id, filename, doc_title(=filename
    /// without extension), topics_rule_extracted(=tags.join(' ')),
    /// authors(=custom_metadata.authors ?? null)}. SPCPipeline uses this as the
    /// sole product write source for the doc-level FTS5 fallback row.
    static nlohmann::json DeriveDocFts5Columns(const GeneratorInput& input);
};

}  // namespace cortrix::metadata
