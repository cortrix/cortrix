#pragma once
#include <cstdint>
#include "cortrix/store/cortrix_store.h"
#include <string>
#include <atomic>
#include <mutex>

struct sqlite3;

namespace cortrix {

/// Sanitize user input for FTS5 MATCH queries.
/// Wraps each word token in double quotes to prevent FTS5 operator injection
/// (AND, OR, NOT, NEAR, *, etc.).  Returns empty string for empty/whitespace input.
std::string SanitizeFts5Query(const std::string& raw_query);

class CortrixStoreSqlite : public CortrixStore {
public:
    /// Open()-time startup responsibilities (D-I1.bis). The connection itself is
    /// always opened in owns mode; these gate the two one-shot startup jobs that
    /// must NOT run per-request: the framework DDL (production units are migrated
    /// once at F05 load time by SchemaMigrator) and doc-level crash recovery
    /// (runs once at F05 load time; per-request it would mis-reset docs that are
    /// legitimately mid-processing on a concurrent request).
    struct OpenOptions {
        bool run_schema_ddl = true;
        bool run_crash_recovery = true;
    };

    /// Owns-the-connection mode, full standalone startup (DDL + crash recovery):
    /// Open() opens `db_path` itself (WAL + FULLMUTEX + busy_timeout et al.) and
    /// Close()/dtor close it. `owns_db_ = true`. Test / standalone callers.
    explicit CortrixStoreSqlite(const std::string& db_path);

    /// Owns-the-connection mode with explicit startup responsibilities. The
    /// production façade passes {false, false}: per-facade PRIVATE connection
    /// (D-I1.bis) — schema already migrated and doc recovery already run once at
    /// F05 load time, so neither one-shot startup job may run per-request.
    CortrixStoreSqlite(const std::string& db_path, OpenOptions options);

    /// External-connection view: borrows an already-open `sqlite3*`. Open() skips
    /// `sqlite3_open` (and DDL/recovery); Close()/dtor never `sqlite3_close` —
    /// the owning SqliteConn does. The borrowed handle must outlive this store.
    /// `owns_db_ = false`.
    ///
    /// ★ D-I1.bis (2026-06-10): SINGLE-THREADED CONTEXTS ONLY (F05 load-time
    /// tooling — e.g. the assembly-time RecoverCrashedDocs pass — and tests).
    /// The former production use (per-request façade views sharing the bundle's
    /// conn across threads) segfaulted under concurrency and is now forbidden:
    /// per-instance mu_ cannot guard a connection shared across instances.
    explicit CortrixStoreSqlite(sqlite3* external_conn);

    ~CortrixStoreSqlite() override;

    int Open() override;
    int Close() override;

    int doc_create(CortrixDoc& doc) override;
    int doc_get(const std::string& doc_id, CortrixDoc& doc) override;
    int doc_update_status(const std::string& doc_id, DocStatus status,
                           const std::string& error_message = "") override;
    int doc_delete(const std::string& doc_id) override;
    int doc_delete_by_source_prefix(const std::string& source_path_prefix,
                                    int64_t* deleted_blocks) override;  // [F16a D3 · P2c]
    int doc_soft_delete(const std::string& doc_id, int64_t now_ms) override;
    int doc_restore(const std::string& doc_id) override;
    int doc_list_deleted_before(int64_t cutoff_ms, std::vector<CortrixDoc>& out) override;
    int doc_list_by_status(DocStatus status, std::vector<CortrixDoc>& out) override;
    int doc_find_by_source(const std::string& source_type,
                            const std::string& source_path,
                            CortrixDoc& doc) override;
    int doc_find_by_hash(const std::string& content_hash,
                          CortrixDoc& doc) override;
    int doc_count(int64_t* count) override;

    int block_insert(CortrixBlock& block) override;
    int block_get(uint64_t block_id, CortrixBlock& block) override;
    int block_get_by_doc(const std::string& doc_id, std::vector<CortrixBlock>& out) override;
    int block_delete_by_doc(const std::string& doc_id) override;
    int block_count(int64_t* count) override;
    int block_count_by_doc(const std::string& doc_id, int64_t* count) override;

    int parent_insert(CortrixParent& parent) override;
    int parent_get(const std::string& parent_id, CortrixParent& out) override;

    int search_fulltext(const std::string& query, int top_k,
                         std::vector<SearchResult>& results) override;
    int search_metadata(const std::string& filter, int top_k,
                         std::vector<SearchResult>& results) override;

    /// D3.5 F03/F07: expose the owned/borrowed sqlite3* so the SPC write path can
    /// persist per-block enrichment (enriched_score / entities, later semantic_score)
    /// on the same connection inside the F25 write. Non-null after Open().
    sqlite3* db_handle() override;

    /// Crash recovery: processing docs -> delete blocks -> reset to pending
    int RecoverCrashedDocs();

    /// Testing seam (F23 §4.5): force the next `n` public CRUD calls (doc_*/
    /// block_*/parent_*/search_*) to fail with -1 without touching the db, to
    /// exercise callers' storage-failure branches. `n == 0` disables it. Not
    /// part of the production path (a single relaxed load when disarmed).
    void SetFailNextOps(int n) { fail_next_ops_.store(n, std::memory_order_relaxed); }

private:
    int ExecutePragma();
    int CreateTables();

    /// Consume one injected op fault if armed (SetFailNextOps seam).
    bool TryConsumeOpFault();

    std::string db_path_;
    sqlite3* db_ = nullptr;
    bool owns_db_ = true;   ///< false ⇒ borrowed conn view (single-threaded contexts, D-I1.bis)
    OpenOptions opts_;      ///< owns-mode startup responsibilities (D-I1.bis)
    std::atomic<int> fail_next_ops_{0};  ///< testing seam (SetFailNextOps)
    mutable std::mutex mu_;
};

}  // namespace cortrix
