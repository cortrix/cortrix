#pragma once
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/doc_summary/doc_summary_types.h"  // DocDiscoveryHit

struct sqlite3;  // opaque, same fwd-decl style as cortrix_store_sqlite.h

namespace cortrix::doc_summary {

/// One F08-field row fed into the doc-level FTS5 index (F41 §4.3). These are the
/// rule-extracted F08 metadata fields — they always exist (no LLM dependency), so
/// the FTS5 path is the hybrid fallback when the LLM summary is missing/failed.
struct DocFtsRow {
    std::string doc_id;
    std::string filename;
    std::string doc_title;
    std::string topics_rule_extracted;   ///< F08 tags joined (space-separated)
    std::string authors;
};

/// One doc-level FTS5 hit (F41 §8.2 fallback path). `score` is a normalized
/// match score in (0,1] derived from the FTS5 bm25() rank (lower bm25 = better →
/// mapped to a higher score) so it can join the RRF fusion uniformly.
struct DocFtsHit {
    std::string doc_id;
    std::string filename;
    std::string doc_title;
    double score = 0.0;
};

/// Borrowed-handle product helpers over the per-Unit `doc_fts5_index` table.
/// These functions do not own/open/migrate SQLite; the caller supplies the Unit
/// store handle after F41SchemaProvider has created the virtual table. They are
/// shared by the product SPC write hook, rollback cleanup, HTTP/query discovery,
/// and the standalone DocFts5Index wrapper below so benchmark and unit paths
/// cannot drift.
Status UpsertDocFts5Row(sqlite3* db, const DocFtsRow& row);
Status DeleteDocFts5Row(sqlite3* db, const std::string& doc_id);
Result<std::vector<DocFtsHit>> SearchDocFts5(sqlite3* db, const std::string& query,
                                             int top_k);

/// Self-contained doc-level FTS5 index (F41 §4.3 / §8.2 hybrid fallback).
///
/// Standalone (D3): owns its own SQLite DB (Open(":memory:") in tests), creating
/// the `doc_fts5_index` virtual table from the F41SchemaProvider DDL SoT so the
/// two never drift. Product code uses the borrowed-handle helpers above against
/// the F05 per-Unit store.db. The query path uses BM25 column weights (§8.2:
/// doc_title 2.0 / topics 1.5 / filename 1.0) and the shared SanitizeFts5Query
/// guard (M-SEC-001) against FTS5 operator injection.
class DocFts5Index {
public:
    DocFts5Index() = default;
    ~DocFts5Index();

    DocFts5Index(const DocFts5Index&) = delete;
    DocFts5Index& operator=(const DocFts5Index&) = delete;

    /// Open `db_path` (":memory:" for tests) and create the doc_fts5_index virtual
    /// table from the F41SchemaProvider DDL SoT. Returns a CX_ERR_F41_* Status on
    /// failure.
    Status Open(const std::string& db_path);

    /// Index (or replace) one document's F08 fields. Idempotent on doc_id
    /// (delete-then-insert) so re-indexing a doc does not duplicate rows.
    Status Upsert(const DocFtsRow& row);

    /// BM25 search over the F08 fields (§8.2 fallback path). `top_k<=0` → no
    /// results. Returns hits sorted by score DESC. An empty / whitespace-only
    /// (post-sanitize) query returns an empty list (not an error). A query-time
    /// SQLite failure surfaces as CX_ERR_F41_FTS5_FALLBACK_FAILED.
    Result<std::vector<DocFtsHit>> Search(const std::string& query, int top_k) const;

    bool is_open() const { return db_ != nullptr; }

private:
    sqlite3* db_ = nullptr;
};

/// RRF-fuse the two doc-discovery recall paths (F41 §8.2, dedup by doc_id). Each
/// input list is already sorted by its own score DESC; only rank position matters.
/// A doc's contribution from a path is 1/(k+rank) (rank 0-based, top hit = 1/k);
/// scores sum across paths. Results are sorted by fused score DESC, truncated to
/// `top_k` (<=0 → all). The llm_summary path wins the metadata when a doc appears
/// in both (its via_path/fields are richer); the fts5 path only supplies docs the
/// summary path missed.
std::vector<DocDiscoveryHit> FuseDocDiscovery(
    const std::vector<DocDiscoveryHit>& llm_summary_hits,
    const std::vector<DocFtsHit>& fts5_hits, int top_k, int k = 0);

}  // namespace cortrix::doc_summary
