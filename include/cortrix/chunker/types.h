#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/id/types.h"
#include "cortrix/spc/parser.h"  // cortrix::spc::DocumentMetadata (parser SoT)

// Parent-Child Chunking — core data structures.
//
// Two-layer chunk model: a coarse Parent (1024t context block) + N fine Children
// (200t retrieval units). Only Children enter the vector index (P-HNSW); the
// Parent is expanded at recall time to feed the LLM. Relationship is flat: a
// Child links back to its Parent via `parent_id` (FK), a Parent enumerates its
// Children via `child_ids`.
//
// IDs are ULIDs (time-ordered, 26 chars) carried as cortrix::id::* aliases
// (= std::string in Phase 1; distinct strong types = Phase 2). embedding lives in
// P-HNSW, never in these structs.
namespace cortrix::chunker {

/// DocumentMetadata is the parser SoT type (cortrix::spc::DocumentMetadata),
/// not redefined here. Parents inherit it from the doc; Children inherit it from
/// their Parent (per-parent granularity, § 3.2 / D9 lock).
using cortrix::spc::DocumentMetadata;

/// Parent chunk — coarse context block persisted in the SQLite `parents` table
/// (§ 3.1). `parent_text` is the full ~1024t span expanded into the LLM context
/// at recall time. metadata carries doc-level fields + (post-enrichment) per-parent
/// NER/Summary; Children inherit it. (detailed design § 2.2)
struct ParentChunk {
    cortrix::id::ParentId parent_id;       ///< ULID, time-ordered
    cortrix::id::DocId    doc_id;          ///< upstream document id
    cortrix::id::NamespaceId namespace_id;
    std::string parent_text;               ///< up to parent_size token text
    uint32_t token_count = 0;              ///< actual token count (≤ parent_size)
    uint32_t page_start = 0;               ///< first source page_num
    uint32_t page_end = 0;                 ///< last source page_num
    uint64_t byte_offset_start = 0;        ///< byte offset within the document
    uint64_t byte_offset_end = 0;
    DocumentMetadata metadata;             ///< inherited from doc-level
    std::vector<cortrix::id::ChildId> child_ids;  ///< the N children of this parent
    int64_t created_at = 0;                ///< Unix epoch ms
};

/// Child chunk — fine retrieval unit. `child_text` (≤ reranker max_seq_length) is the
/// reranker / embedding input. metadata is inherited from the Parent (D9 lock:
/// child does not generate its own NER/Summary). The embedding is NOT stored
/// here — it goes to P-HNSW keyed by child_id with parent_id in vec metadata.
/// (detailed design § 2.2)
struct ChildChunk {
    cortrix::id::ChildId  child_id;        ///< ULID, time-ordered
    cortrix::id::ParentId parent_id;       ///< FK → ParentChunk.parent_id
    cortrix::id::DocId    doc_id;
    cortrix::id::NamespaceId namespace_id;
    std::string child_text;                ///< ≤ reranker max_seq_length token text
    uint32_t token_count = 0;
    uint32_t parent_offset = 0;            ///< start offset of child_text inside parent_text
    uint32_t chunk_index = 0;              ///< order of this child within the doc (0,1,2,...)
    DocumentMetadata metadata;             ///< inherited from parent
    int64_t created_at = 0;
    // embedding is not in this struct — it lives in P-HNSW.
};

/// Chunking statistics surfaced into the Agent-friendly response `meta.stats`
/// (§ 5.3). Tracks document-parse completeness + chunk counts; `failed_pages`
/// carries the per-page warnings the parser reported (the chunker inherits per-page tolerance,
/// § 4.5).
struct ChunkerStats {
    uint32_t total_pages = 0;              ///< pages received from the parser
    uint32_t succeeded_pages = 0;          ///< pages with usable content
    uint32_t failed_pages = 0;             ///< pages the parser flagged failed (skipped)
    uint32_t total_parents = 0;
    uint32_t total_children = 0;
    uint32_t empty_parents = 0;            ///< parents skipped (all-empty paragraphs, § 4.5)
    bool fallback_to_flat = false;         ///< flat fallback triggered (§ 4.5 / D4 lock)

    /// succeeded_pages / total_pages, clamped to [0,1]; 1.0 when total_pages == 0.
    double CoverageRatio() const {
        if (total_pages == 0) return 1.0;
        return static_cast<double>(succeeded_pages) / static_cast<double>(total_pages);
    }
};

}  // namespace cortrix::chunker
