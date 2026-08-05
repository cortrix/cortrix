#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/spc/parser.h"

// Metadata Block — data structures.
//
// net-new namespace cortrix::metadata (verified dev=8e189e1 has no such ns/dir, so creating it has no conflict).
// One document-level Metadata Block (Chunk[0]) is generated per document, holding
// rule-extracted structured metadata (no LLM dependency, D4 lock). 26-field schema
// (23 + custom_metadata = 24 + the cleaning-coordination meta.parse_status /
// meta.parse_failed_page = 26 final).
//
// Frozen-contract consumption: the ONLY consumed parser type is
// cortrix::spc::DocumentMetadata (parser.h:75) — its real field names are
// filename / doc_title / mime_type / file_size_bytes / page_count / doc_language /
// upload_timestamp / parse_time_ms (NOT the stale `lang`/`upload_time` of detailed design §2.1).
// FileInfo / ProcessingStats below are this layer's OWN GeneratorInput DTOs
// (defined here only) — they carry the request-level / pipeline-stats inputs
// the parser's DocumentMetadata does not (source_uri / tags / parser_name / counts).
namespace cortrix::metadata {

/// File-origin info (owned DTO — NOT a parser type, `file_info`). The
/// request-level facts about the uploaded file that DocumentMetadata does not
/// carry: where it came from, the business-set tags, and the upload instant. mime
/// type / size / filename are the authoritative DocumentMetadata copies, so they
/// are not duplicated here (the generator reads those from doc_metadata).
struct FileInfo {
    std::string source_uri;          ///< "file:///uploads/x.pdf" — request origin (source of business fields)
    std::vector<std::string> tags;   ///< tags[] set by the business side at upload time (D9 B' lock, set once)
    int64_t upload_time_ms = 0;      ///< Upload timestamp (Unix epoch ms) — schema upload_time
};

/// Parse / pipeline statistics (owned DTO — NOT a parser type,
/// `stats`). Carries the producer/version provenance + chunk counts the schema's
/// A-class system fields need. parent_count / child_count come from the chunker
/// (parse → chunk → metadata, so they only exist after splitting); standalone these are supplied by the caller /
/// mock. parse_status / parse_failed_page are the cleaning-coordination signals passed through from the parser
/// (V2 rework M-03), surfaced here so the generator can fill meta.parse_status /
/// meta.parse_failed_page without re-deriving them.
struct ProcessingStats {
    int page_count = -1;             ///< -1 = unknown (→ CX_WARN_METADATA_PARTIAL)
    int chunk_count = -1;            ///< total chunks (= child_count in V1.0)
    int parent_count = -1;           ///< parent-chunk count (the chunker runs first)
    int child_count = -1;            ///< child-chunk count
    int64_t processing_time_ms = 0;  ///< parse + split duration

    std::string parser_name;         ///< ParsedDoc.parser_name ("docling"/"paddleocr"/...)
    std::string parser_version;      ///< Parser version
    std::string enricher_name;       ///< "llm" / "null" (NullEnricher = L0 fallback)
    std::string enricher_version;    ///< Enricher version
    std::string chunker_name;        ///< "parent-child"
    std::string chunker_version;     ///< Chunker version

    /// meta.parse_status: "ok" | "failed" | "partial". Passed through from the parser
    /// (not settable by the business side). Empty string is normalized to "ok" by the generator.
    std::string parse_status = "ok";
    /// meta.parse_failed_page (V2 rework M-03): the page number in ParsedDoc.failed_pages
    /// this chunk sits on, or -1 when none (serialized as null unless failed/partial).
    int parse_failed_page = -1;
};

/// The document-level Metadata Block (detailed design §2.2). One per document. block_text is the
/// embedding input (D3 lock: a natural-language sentence, 7-10 core fields); metadata_json is the full
/// 26-field JSON (D2/D9 + V2 rework); embedding is filled when the block enters P-HNSW
/// (D7 lock — left empty standalone, real embedding is D3.5 pipeline wiring).
struct MetadataBlock {
    std::string block_id;              ///< ULID (id/ulid.h, produced by the chunker — reuse, do not recreate)
    std::string doc_id;                ///< Associated doc (cortrix::id::DocId = string)
    std::string namespace_id;
    std::string block_text;            ///< D3 lock: natural-language sentence (embedding input)
    nlohmann::json metadata_json;      ///< Full 26-field JSON (D2/D9 + V2 rework)
    std::vector<float> embedding;      ///< embedding(block_text) — D7 lock; empty standalone → D3.5

    // D9 B' compromise: V1.0 schema immutable (the business side sets custom_metadata once at upload time);
    // Phase 2 adds version management + an update API (Block Versioning + Update API).
    // Hotness fields are not in this Block (per-doc single-Block hotness stats are of little value; hotness lives in the parent/child chunk rows).
};

/// Generator input. doc_metadata is the consumed parser type; file_info /
/// stats / custom_metadata are metadata-owned request/pipeline inputs.
struct GeneratorInput {
    std::string doc_id;
    std::string namespace_id;
    cortrix::spc::DocumentMetadata doc_metadata;  ///< parser output (parser.h:75, real fields)
    FileInfo file_info;                           ///< metadata-owned DTO (source/tags/upload time)
    ProcessingStats stats;                        ///< metadata-owned DTO (provenance/counts)

    /// D9 B' lock: custom key-value set once by the business side at upload time (arbitrary JSON object). Empty object by default.
    nlohmann::json custom_metadata = nlohmann::json::object();
};

/// Generator output (detailed design §2.1). Carries the block plus the partial-success
/// warnings the §5.3 body surfaces (coverage_ratio / fields_missing) so a partial
/// generation returns HTTP 200 + warnings rather than an error.
struct GeneratorOutput {
    MetadataBlock block;

    /// Fields that could not be filled (e.g. page_count unknown). Empty = full
    /// coverage. Non-empty → the caller attaches CX_WARN_METADATA_PARTIAL +
    /// coverage_ratio to the §5.3 partial-success meta block.
    std::vector<std::string> missing_fields;
};

/// Total field count of the V1.0 metadata_json schema (detailed design §3.1 — D2 23 + D9 1 +
/// V2 rework 2 = 26). Compile-time anchor for the coverage_ratio denominator + the
/// schema-completeness regression test (the field set must not silently shrink).
constexpr int kMetadataFieldCount = 26;

}  // namespace cortrix::metadata
