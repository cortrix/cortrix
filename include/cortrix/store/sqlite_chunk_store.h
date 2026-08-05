#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/store/chunk_store.h"

struct sqlite3;

namespace cortrix::store {

/// Phase 1 ChunkStore implementation over the unified `blocks` table (A unified
/// blocks). Child chunks are `blocks` rows with
/// child_id IS NOT NULL; their content_text holds the chunk text and the
/// parent_id / chunk_index are F34-owned columns (ParentChildSchemaProvider ALTERs). It is
/// a read-only reverse-lookup:
///   Get / GetBatch    — by child_id (the P-HNSW label's business ULID)
///   GetChunksByDocId  — all child chunks of a doc, ascending chunk_index
///
/// BORROWS a sqlite3* handle (the per-Unit DB, e.g. CortrixStore::db_handle()); it
/// does NOT open / own / close the DB — the block-header framework + ParentChildSchemaProvider own
/// the blocks schema, and the handle must outlive this store. Thread-safe via an
/// internal mutex (defense-in-depth alongside SQLite FULLMUTEX), mirroring
/// SqliteParentChunkStore. The store-layer unified error model (store_errors.h)
/// applies: NOT_FOUND is permanent, DB_ERROR is transient.
class SqliteChunkStore : public ChunkStore {
public:
    /// @param db  borrowed per-Unit sqlite3 handle (non-owning; must outlive this).
    explicit SqliteChunkStore(sqlite3* db);

    /// Single child lookup. Ok(ChunkRecord) on hit; CX_ERR_STORE_NOT_FOUND when
    /// child_id is absent (store-layer unified error model); CX_ERR_STORE_DB_ERROR
    /// on a null handle / SQLite error.
    Result<ChunkRecord> Get(const std::string& child_id) override;

    /// Batch lookup. Output order matches input order; missing child_ids are
    /// skipped and appended to *missing_ids when provided. A null handle / prepare
    /// failure routes every id to *missing_ids and returns empty.
    std::vector<ChunkRecord> GetBatch(const std::vector<std::string>& child_ids,
                                      std::vector<std::string>* missing_ids = nullptr) override;

    /// All child chunks of a document, ascending by chunk_index (doc-summary
    /// map-reduce input). Non-child rows (META / doc_summary, child_id IS NULL) and
    /// other docs are excluded. Empty when the doc has no child chunks.
    std::vector<ChunkRecord> GetChunksByDocId(const std::string& doc_id) override;

private:
    void EnsureScoreColumnCacheLocked();

    sqlite3* db_;       // borrowed (non-owning)
    std::mutex mu_;
    bool score_columns_checked_ = false;
    bool has_enriched_score_ = false;
    bool has_semantic_score_ = false;
};

}  // namespace cortrix::store
