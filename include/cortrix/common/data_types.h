#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace cortrix {

enum class DocStatus {
    kPending    = 0,
    kProcessing = 1,
    kReady      = 2,
    kError      = 3,
    kStale      = 4,
    kDeleting   = 5,
    kDeleted    = 6,  // OPEN-2 Stage 1 soft delete (restorable for soft_delete_retention_days)
};

struct CortrixDoc {
    std::string doc_id;            // ULID (D-I6); app-minted in doc_create
    std::string source_type;
    std::string source_path;
    std::string source_ref;
    std::string content_hash;
    int64_t     file_size = 0;
    std::string mime_type;
    DocStatus   status = DocStatus::kPending;
    std::string error_message;
    int         processing_level = 3;
    int64_t     block_count = 0;
    std::string chunk_strategy;
    std::string created_at;
    std::string updated_at;
    std::string processed_at;
    // CDC-specific fields (NULL for non-CDC sources)
    std::string cdc_schema;      // CDC: table schema snapshot (JSON)
    std::string cdc_position;    // CDC: sync position (PostgreSQL LSN / MySQL binlog pos)
    std::string cdc_last_sync;   // CDC: last sync time
    // Extension fields
    std::string title;
    std::string language;
    std::string metadata_json;
};

struct CortrixBlock {
    uint64_t              block_id = 0;
    std::string          doc_id;       // ULID (D-I6) FK → documents.doc_id
    int                  chunk_index = 0;
    int                  block_type = 0;
    int                  processing_level = 0;
    int64_t              hnsw_node_id = -1;
    std::string          content_hash;
    std::vector<uint8_t> data;
    std::string          content_text;
    // [A unified-blocks] child-row + shared columns (F34/F35/F40 SchemaProviders own
    // the DDL; this struct carries them through block_insert/block_get). Empty string
    // / -1 sentinel → SQL NULL (a non-child block leaves child_id/parent_id empty and
    // token_count/parent_offset = -1). A child row = child_id non-empty; its block_type
    // stays the source modality (candidate B, no kBlockChild).
    std::string          child_id;        // F34: business ULID + child-identity key (empty = not a child)
    std::string          parent_id;       // F34: FK → parents.parent_id (empty = NULL)
    int64_t              token_count = -1;   // F34: child token count (-1 = NULL)
    int64_t              parent_offset = -1; // F34: offset within parent_text (-1 = NULL)
    std::string          metadata_json;   // F09-framework shared JSONB (MEM/F34 child/F08 META); empty = NULL
};

// [A unified-blocks] Storage DTO for the per-Unit `parents` table (F34 § 3.1).
// parents are KEPT under A (child chunks moved into `blocks`; parents did not), and
// CortrixParent is the persistence twin of the pipeline-side cortrix::chunker::
// ParentChunk — exactly as CortrixBlock is the storage twin of chunker::ChildChunk.
// The SPC layer converts ParentChunk → CortrixParent (its DocumentMetadata serialized
// into the opaque metadata_json; the non-persisted child_ids dropped) so the store
// interface stays free of chunker / parser / id-strong-type dependencies. The three
// D8 hotness columns (access_count / last_access_at / hotness_score) are reserved —
// V1.0 neither writes nor reads them; the DDL DEFAULTs fill them — so they are not on
// this struct. All columns are NOT NULL in the DDL, so every field is always bound.
struct CortrixParent {
    std::string parent_id;          // ULID PK
    std::string doc_id;             // FK → documents.doc_id
    std::string namespace_id;
    std::string parent_text;        // full ~1024t span (recall-time LLM expansion)
    int64_t     token_count = 0;
    int64_t     page_start = 0;
    int64_t     page_end = 0;
    int64_t     byte_offset_start = 0;
    int64_t     byte_offset_end = 0;
    std::string metadata_json;      // opaque JSON (F06 doc meta + post-F03 per-parent)
    int64_t     created_at = 0;     // Unix epoch ms
};

struct SearchResult {
    uint64_t     block_id = 0;
    std::string doc_id;            // ULID (D-I6)
    double      score = 0.0;
    std::string content_text;
};

struct VectorResult {
    uint64_t block_id = 0;
    float   distance = 0.0f;
};

}  // namespace cortrix
