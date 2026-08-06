#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/chunker/types.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/store/parent_chunk_store.h"
#include "cortrix/store/parent_chunk_store_metrics.h"

struct sqlite3;

// SqliteParentChunkStore — the default ParentChunkStore.
//
// Persists parents + children to SQLite (schema) and serves the parent
// reverse-lookups. Standalone: it owns its own sqlite3 handle so chunker unit-tests
// run against a real (in-memory or temp-file) DB without the PWL / metadata / index
// wiring (that single-transaction integration is integration). The LRU cache layer
// (Phase-2 anchor TD-PARENT-CHUNK-STORE-CACHE) is not implemented in V1.0.
namespace cortrix::store {

class SqliteParentChunkStore : public ParentChunkStore {
public:
    /// @param db_path  SQLite file path, or ":memory:" for an in-process DB.
    /// @param metrics  optional lookup-metrics recorder. nullptr (the
    ///   default — backward-compatible with the original single-arg ctor) binds
    ///   the process-wide ParentChunkStoreMetrics::Instance(); tests inject a
    ///   private recorder for isolated assertions.
    explicit SqliteParentChunkStore(std::string db_path,
                                    ParentChunkStoreMetrics* metrics = nullptr);
    ~SqliteParentChunkStore() override;

    SqliteParentChunkStore(const SqliteParentChunkStore&) = delete;
    SqliteParentChunkStore& operator=(const SqliteParentChunkStore&) = delete;

    /// Open the DB + run the schema migration (CREATE TABLE IF NOT EXISTS
    /// parents/children + indexes). Idempotent. OK or CX_ERR_STORE_DB_ERROR.
    Status Open();
    void Close();

    // --- write path (standalone — real PWL single-txn wiring = integration) -----

    /// Insert one parent + its children in a single SQLite transaction (the
    /// PWL BeginWrite→Commit boundary wraps this at integration; here it is a local txn so
    /// a partial failure rolls back). metadata is JSON-serialized into
    /// metadata_json. OK or CX_ERR_STORE_DB_ERROR.
    Status PutParentWithChildren(const cortrix::chunker::ParentChunk& parent,
                                 const std::vector<cortrix::chunker::ChildChunk>& children);

    /// Convenience: persist a whole ChunkerOutput (each parent + its children) in
    /// one transaction. Flat-fallback children (empty parent_id) are inserted into
    /// children only. OK or CX_ERR_STORE_DB_ERROR.
    Status PutChunkerOutput(const std::vector<cortrix::chunker::ParentChunk>& parents,
                            const std::vector<cortrix::chunker::ChildChunk>& children);

    // --- read path (ParentChunkStore) -------------------------------------------

    Result<cortrix::chunker::ParentChunk> GetParent(const std::string& parent_id) override;
    Result<std::vector<cortrix::chunker::ParentChunk>> BulkGetParents(
        const std::vector<std::string>& parent_ids) override;

private:
    Status Migrate();  // CREATE TABLE IF NOT EXISTS ...

    std::string db_path_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mu_;
    ParentChunkStoreMetrics* metrics_;  // never null after ctor (Instance() fallback)
};

}  // namespace cortrix::store
