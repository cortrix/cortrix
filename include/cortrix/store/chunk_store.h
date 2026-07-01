#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/score_signals.h"

namespace cortrix::store {

/// Child-level chunk content reverse-lookup record (F02 §2.1-bis SoT).
struct ChunkRecord {
    std::string child_id;     ///< ULID, 26 chars
    std::string parent_id;    ///< doc-level parent block id
    std::string content;      ///< chunk text
    std::int32_t chunk_index = 0;  ///< chunk order within the doc (F34 parent-child mapping)
    ScoreSignals score_signals;   ///< optional F03/F07 query-time scoring signals
};

/// Child-level chunk content reverse-lookup interface (scaffolding D2-pre-8,
/// interface SoT = F02 §2.1-bis). Consumed by F02 Rerank, F04 ScatterGather, and
/// F37 CRAG. The Phase 1 concrete implementation is SqliteChunkStore over the
/// unified `blocks` table (A unified blocks, 2026-06-08, F02 §2.1-bis SoT): child
/// chunks are blocks rows with child_id IS NOT NULL; content_text holds the chunk
/// text and parent_id/chunk_index are F34-owned columns — a single-table read, no
/// F08 META + F09 blob assembly (the pre-A-blocks approach). Tests use MockChunkStore.
/// F34 ParentChunkStore is a *parallel* parent-level lookup (separate class, not
/// derived from ChunkStore).
class ChunkStore {
public:
    virtual ~ChunkStore() = default;

    /// Single chunk lookup. Returns CX_ERR_STORE_NOT_FOUND when child_id is absent
    /// (store-layer unified error model, store_errors.h).
    virtual Result<ChunkRecord> Get(const std::string& child_id) = 0;

    /// Batch lookup (F02 Rerank main path). Output order matches input order;
    /// missing child_ids are skipped and appended to *missing_ids when provided.
    virtual std::vector<ChunkRecord> GetBatch(const std::vector<std::string>& child_ids,
                                              std::vector<std::string>* missing_ids = nullptr) = 0;

    /// All child chunks of a document, ascending by chunk_index (F41 doc-summary
    /// map-reduce, V14 JF1 ← MVP block_get_by_doc). Empty when the doc has none.
    virtual std::vector<ChunkRecord> GetChunksByDocId(const std::string& doc_id) = 0;
};

}  // namespace cortrix::store
