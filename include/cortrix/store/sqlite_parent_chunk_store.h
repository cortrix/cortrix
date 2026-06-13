#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/chunker/types.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/store/parent_chunk_store.h"

struct sqlite3;

// F34 SqliteParentChunkStore — the default ParentChunkStore (detailed design § 2.5 / § 3.1).
//
// Persists parents + children to SQLite (§ 3.1 schema) and serves the parent
// reverse-lookups. Standalone: it owns its own sqlite3 handle so F34 unit-tests
// run against a real (in-memory or temp-file) DB without the F25 PWL / F08 / F01
// wiring (that single-transaction integration is D3.5). The LRU cache layer
// (§ 2.5 Phase-2 anchor TD-PARENT-CHUNK-STORE-CACHE) is not implemented in V1.0.
namespace cortrix::store {

class SqliteParentChunkStore : public ParentChunkStore {
public:
    /// @param db_path  SQLite file path, or ":memory:" for an in-process DB.
    explicit SqliteParentChunkStore(std::string db_path);
    ~SqliteParentChunkStore() override;

    SqliteParentChunkStore(const SqliteParentChunkStore&) = delete;
    SqliteParentChunkStore& operator=(const SqliteParentChunkStore&) = delete;

    /// Open the DB + run the § 3.1 schema migration (CREATE TABLE IF NOT EXISTS
    /// parents/children + indexes). Idempotent. OK or CX_ERR_STORE_DB_ERROR.
    Status Open();
    void Close();

    // --- write path (§ 4.2; standalone — real PWL single-txn wiring = D3.5) -----

    /// Insert one parent + its children in a single SQLite transaction (the F25
    /// PWL BeginWrite→Commit boundary wraps this at D3.5; here it is a local txn so
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
};

}  // namespace cortrix::store
