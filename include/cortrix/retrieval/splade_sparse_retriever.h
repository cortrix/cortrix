#pragma once
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/id/types.h"
#include "cortrix/retrieval/sparse_codec.h"
#include "cortrix/retrieval/sparse_retriever.h"

struct sqlite3;

namespace cortrix::retrieval {

/// SpladeSparseRetriever configuration. Top-K + the
/// posting-list caps + LRU cache size. NS-config (sparse_top_k 50-200, S7)
/// overrides default_top_k at resolve time; the caps are A-class (process
/// global), not NS-overridable.
struct SpladeConfig {
    int default_top_k = 100;          ///< K=100 default (NS 50-200)
    int posting_list_cap = 1000;      ///< per-term posting cap, weight DESC (§6.5)
    size_t posting_cache_capacity = 1024;  ///< LRU: max cached posting lists (§6 S6)
};

/// SPLADE inverted-index sparse retriever (Phase 1 direct impl).
/// Owns a SQLite-backed per-(ns, term) inverted index; Add/Remove maintain it,
/// Search accumulates SPLADE dot-product scores over the query terms' posting
/// lists with a per-term LRU cache.
///
/// Standalone (D3): owns its own sqlite3 handle (":memory:" or temp file) so the
/// algorithm is unit-testable without the write coordinator / namespace pool / children
/// wiring (that single-DB, FK-to-children, atomic-write integration = D3.5). The
/// detailed design's `shared_ptr<SQLiteHandle>` ctor param does not exist in the
/// frozen tree (there is no SQLiteHandle type) — standalone takes a db_path and
/// owns the connection (mirrors SqliteParentChunkStore); injecting the pooled
/// pool connection is D3.5.
class SpladeSparseRetriever : public ISparseRetriever {
public:
    /// @param config  top-K + caps + cache size.
    /// @param db_path SQLite file path, or ":memory:" for an in-process DB.
    SpladeSparseRetriever(SpladeConfig config, std::string db_path);
    ~SpladeSparseRetriever() override;

    SpladeSparseRetriever(const SpladeSparseRetriever&) = delete;
    SpladeSparseRetriever& operator=(const SpladeSparseRetriever&) = delete;

    /// Open the DB + create the sparse_inverted_index table + index (idempotent).
    /// Must be called before Add/Remove/Search. OK or
    /// CX_ERR_SPARSE_INVERTED_INDEX_WRITE_FAILED (open/DDL fault).
    Status Open();
    void Close();

    // --- ISparseRetriever ---

    /// Query top-K (§6.3): for each query term, read its posting list (LRU
    /// cached), accumulate query_weight × chunk_weight per child, return top-K by
    /// score DESC. `top_k <= 0` uses config.default_top_k. On an internal fault
    /// returns an empty vector + flips last_search_failed() (L2 fallback signal,
    /// §7.2) — never throws.
    std::vector<SparseHit> Search(const SparseVector& query,
                                  const NamespaceId& ns_id, int top_k) override;

    /// Incremental write (§6.1): replace `child_id`'s postings with `vec` (delete
    /// then insert, in one txn). Empty `vec` = remove (§6.5). Invalidates the
    /// affected terms' cache entries. OK or
    /// CX_ERR_SPARSE_INVERTED_INDEX_WRITE_FAILED (retryable).
    Status Add(const NamespaceId& ns_id, const ChildId& child_id,
               const SparseVector& vec) override;

    /// Incremental delete (§6.2): drop all postings for `child_id` in `ns_id`.
    /// Idempotent. OK or CX_ERR_SPARSE_INVERTED_INDEX_WRITE_FAILED.
    Status Remove(const NamespaceId& ns_id, const ChildId& child_id) override;

    /// True once Open() has succeeded and the DB handle is live.
    bool IsAvailable() const override;

    // --- observability hooks (S6/S9; metric wiring lands in S10) ---

    /// Whether the most recent Search hit an internal fault (→ caller takes L2
    /// fallback, §7.2). Reset at the start of each Search.
    bool last_search_failed() const;

    /// Posting-list LRU cache hit-rate since construction (S6 / §verify ≥70%).
    double cache_hit_rate() const;

private:
    struct PostingList {
        std::vector<std::pair<ChildId, float>> entries;  // (child_id, weight)
    };

    // Per-(ns, term) posting-list LRU. key = ns_id + '\0' + term_id.
    std::string CacheKey(const NamespaceId& ns_id, uint32_t term_id) const;
    const PostingList& GetPostingList(const NamespaceId& ns_id, uint32_t term_id);
    void InvalidateTerm(const NamespaceId& ns_id, uint32_t term_id);

    SpladeConfig config_;
    std::string db_path_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mu_;

    // LRU: list front = most-recently-used. map → iterator into the list.
    std::list<std::pair<std::string, PostingList>> lru_;
    std::unordered_map<std::string, decltype(lru_)::iterator> cache_index_;

    mutable uint64_t cache_hits_ = 0;
    mutable uint64_t cache_misses_ = 0;
    bool last_search_failed_ = false;
};

}  // namespace cortrix::retrieval
