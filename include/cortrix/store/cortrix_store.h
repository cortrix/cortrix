#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "cortrix/common/data_types.h"

// Opaque SQLite handle (forward-declared C type) — see CortrixStore::db_handle().
struct sqlite3;

namespace cortrix {

/// Metadata + full-text search vtable interface.
/// Return value convention: 0 = success, -1 = generic error, -2 = not found, -3 = already exists
class CortrixStore {
public:
    virtual ~CortrixStore() = default;

    // ---- Lifecycle ----
    virtual int Open() = 0;
    virtual int Close() = 0;

    // ---- Document CRUD ----
    virtual int doc_create(CortrixDoc& doc) = 0;
    virtual int doc_get(const std::string& doc_id, CortrixDoc& doc) = 0;
    virtual int doc_update_status(const std::string& doc_id, DocStatus status,
                                   const std::string& error_message = "") = 0;
    virtual int doc_delete(const std::string& doc_id) = 0;
    // Full-overwrite import cleanup: delete every document whose
    // source_path begins with `source_path_prefix` (LIKE 'prefix%', uses
    // idx_doc_source) together with its blocks (FTS cleaned via the blocks_ad
    // trigger), in one transaction. Writes the count of blocks removed to
    // *deleted_blocks (0 if none / null arg ignored). The prefix MUST end at a
    // table boundary so "users/" never matches "users2/" (R6). 0 on success, -1 on
    // a store error. Default returns -1 so lightweight fakes need not override.
    virtual int doc_delete_by_source_prefix(const std::string& source_path_prefix,
                                            int64_t* deleted_blocks) {
        (void)source_path_prefix;
        if (deleted_blocks) *deleted_blocks = 0;
        return -1;
    }

    // ---- OPEN-2 three-stage GC (default impls return -1 so lightweight fakes
    //      need not override; CortrixStoreSqlite implements them). ----
    /// Stage 1 soft delete: set status='deleted' + deleted_at=now_ms. 0 ok / -2 not found.
    virtual int doc_soft_delete(const std::string& doc_id, int64_t now_ms) {
        (void)doc_id; (void)now_ms; return -1;
    }
    /// Stage 1 reversal (restore): clear deleted_at + set status='ready' (only if the
    /// doc is currently 'deleted'). 0 ok / -2 not found or not soft-deleted.
    virtual int doc_restore(const std::string& doc_id) { (void)doc_id; return -1; }
    /// Stage 2 candidates: soft-deleted docs whose deleted_at <= cutoff_ms. 0 ok.
    virtual int doc_list_deleted_before(int64_t cutoff_ms, std::vector<CortrixDoc>& out) {
        (void)cutoff_ms; (void)out; return -1;
    }
    virtual int doc_list_by_status(DocStatus status,
                                    std::vector<CortrixDoc>& out) = 0;
    virtual int doc_find_by_source(const std::string& source_type,
                                    const std::string& source_path,
                                    CortrixDoc& doc) = 0;
    virtual int doc_find_by_hash(const std::string& content_hash,
                                  CortrixDoc& doc) = 0;
    virtual int doc_count(int64_t* count) = 0;

    // ---- Block CRUD ----
    virtual int block_insert(CortrixBlock& block) = 0;
    virtual int block_get(uint64_t block_id, CortrixBlock& block) = 0;
    virtual int block_get_by_doc(const std::string& doc_id,
                                  std::vector<CortrixBlock>& out) = 0;
    virtual int block_delete_by_doc(const std::string& doc_id) = 0;
    virtual int block_count(int64_t* count) = 0;
    // Count blocks for a specific document (default impl returns 0)
    virtual int block_count_by_doc(const std::string& doc_id, int64_t* count) {
        (void)doc_id; *count = 0; return 0;
    }

    // ---- Parent CRUD (`parents` table) ----
    // parents is a per-Unit table KEPT under A (child chunks moved to `blocks`). The
    // SPC write path inserts parents on the SAME store.db handle inside the SAME
    // transaction as the doc's child-blocks (ARCH §3.2 atomic write), so the write
    // lives on this unified store rather than a second handle/wrapper. Default impls
    // return -1 (not implemented) so the many lightweight test fakes/stubs need not
    // override. Return convention: 0 ok, -1 error, -2 not found, -3 already exists.
    virtual int parent_insert(CortrixParent& parent) { (void)parent; return -1; }
    virtual int parent_get(const std::string& parent_id, CortrixParent& out) {
        (void)parent_id; (void)out; return -1;
    }

    // ---- Full-text search (FTS5) ----
    virtual int search_fulltext(const std::string& query, int top_k,
                                 std::vector<SearchResult>& results) = 0;

    // ---- Metadata filter (Phase 1) ----
    // MVP returns -1 (not implemented), Query Engine does post-filter in memory.
    virtual int search_metadata(const std::string& filter, int top_k,
                                 std::vector<SearchResult>& results) = 0;

    // ---- Raw SQLite handle (enrichment persistence) ----
    // The SPC ingest write persists per-block enrichment — enriched_score +
    // entities (enricher_store.h), later semantic_score — by calling the free
    // functions over this underlying handle, inside the same logical write as
    // block_insert. Only the SQLite-backed store returns a real handle; every other
    // impl (test fakes, future backends) returns nullptr and the caller skips the
    // enrichment write (the core block row is already persisted via block_insert).
    // sqlite3 is an opaque forward-declared C type, so exposing it pulls no
    // strong-typed Feature dependency into the store interface (β layering kept).
    virtual sqlite3* db_handle() { return nullptr; }
};

}  // namespace cortrix
