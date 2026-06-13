#pragma once
#include <string>
#include <cstdint>
#include <atomic>

namespace cortrix {

enum class SPCPriority : int {
    kP0 = 0,   // Highest: user direct upload (HTTP)
    kP1 = 1,   // Medium: watch_dir changes
    kP2 = 2,   // Low: batch import / large files (>100MB)
};

enum class SPCStage : int {
    kQueued     = 0,
    kParsing    = 1,
    kChunking   = 2,
    kEmbedding  = 3,
    kAssembling = 4,
    kDone       = 5,
    kError      = 6,
    kCancelled  = 7,
};

struct SPCTask {
    int64_t task_id = 0;
    std::string doc_id;            // ULID (D-I6)
    std::string namespace_name;

    std::string source_path;
    std::string source_type;
    std::string content_hash;
    std::string mime_type;

    uint8_t processing_level = 3;  // L0=0, L1=1, L2=2, L3=3 (default L3)

    SPCPriority priority = SPCPriority::kP1;
    int64_t created_at_us = 0;
    int64_t file_size = 0;
    // atomic like `cancelled` below: the pipeline worker advances it while
    // submitters (and tests) poll progress cross-thread; a plain enum there
    // is a data race (TSAN-caught).
    std::atomic<SPCStage> stage{SPCStage::kQueued};

    std::atomic<bool> cancelled{false};

    std::string error_message;

    // Note: content_text is intentionally NOT a field of SPCTask.
    // Parsed text is a transient local in SPCPipeline::ProcessTask(), flowing
    // through ParseResult -> ChunkResult -> CortrixBlock.content_text.
    // SPCTask only carries routing/scheduling metadata, not content payload.

    /// Metadata JSON to be stored in block records.
    /// For memory blocks: contains interaction_id, turn, query_type, result_source, ttl_seconds.
    /// For upload blocks: contains user-defined metadata from upload request.
    /// Flows through pipeline: SPCTask -> SPCPipeline -> BlockAssembler -> block BLOB.
    std::string metadata_json;

    bool is_update = false;
    std::string old_doc_id;        // ULID (D-I6)
};

}  // namespace cortrix
