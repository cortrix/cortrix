#include "cortrix/doc_summary/doc_fts5_index.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "cortrix/doc_summary/doc_summary_config.h"
#include "cortrix/doc_summary/doc_summary_error.h"
#include "cortrix/doc_summary/f41_schema_provider.h"  // kDocFts5IndexDdl SoT
#include "cortrix/store/cortrix_store_sqlite.h"        // SanitizeFts5Query (M-SEC-001)

namespace cortrix::doc_summary {

DocFts5Index::~DocFts5Index() {
    if (db_) sqlite3_close(db_);
}

Status DocFts5Index::Open(const std::string& db_path) {
    if (db_) return Status::Ok();  // already open
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return DocSummaryStatus(DocSummaryErrorCode::kFts5FallbackFailed,
                                "open doc_fts5_index db: " + msg);
    }
    // Create the virtual table from the F41SchemaProvider DDL SoT (never drift).
    char* err = nullptr;
    rc = sqlite3_exec(db_, kDocFts5IndexDdl, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "DDL failed";
        sqlite3_free(err);
        sqlite3_close(db_);
        db_ = nullptr;
        return DocSummaryStatus(DocSummaryErrorCode::kFts5FallbackFailed,
                                "create doc_fts5_index: " + msg);
    }
    return Status::Ok();
}

Status DocFts5Index::Upsert(const DocFtsRow& row) {
    if (!db_) {
        return DocSummaryStatus(DocSummaryErrorCode::kFts5FallbackFailed,
                                "Upsert on closed index");
    }
    // Idempotent on doc_id: delete-then-insert (FTS5 external-content tables have
    // no UPSERT; doc_id is UNINDEXED but still a column we can filter on).
    {
        const char* del = "DELETE FROM doc_fts5_index WHERE doc_id = ?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, del, -1, &st, nullptr) != SQLITE_OK) {
            return DocSummaryStatus(DocSummaryErrorCode::kFts5FallbackFailed,
                                    std::string("prepare delete: ") + sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(st, 1, row.doc_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    const char* ins =
        "INSERT INTO doc_fts5_index(doc_id, filename, doc_title, "
        "topics_rule_extracted, authors) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
        return DocSummaryStatus(DocSummaryErrorCode::kFts5FallbackFailed,
                                std::string("prepare insert: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(st, 1, row.doc_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, row.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, row.doc_title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, row.topics_rule_extracted.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, row.authors.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return DocSummaryStatus(DocSummaryErrorCode::kFts5FallbackFailed,
                                std::string("insert: ") + sqlite3_errmsg(db_));
    }
    return Status::Ok();
}

Result<std::vector<DocFtsHit>> DocFts5Index::Search(const std::string& query,
                                                    int top_k) const {
    std::vector<DocFtsHit> hits;
    if (top_k <= 0) return hits;
    if (!db_) {
        return DocSummaryStatus(DocSummaryErrorCode::kFts5FallbackFailed,
                                "Search on closed index");
    }
    // Sanitize against FTS5 operator injection (M-SEC-001, shared helper). An
    // empty / whitespace-only query returns no results (not an error).
    std::string sanitized = cortrix::SanitizeFts5Query(query);
    if (sanitized.empty()) return hits;

    // §8.2 BM25 column weights: filename 1.0 / doc_title 2.0 / topics 1.5 /
    // authors 1.0. doc_id is UNINDEXED (weight 0). bm25() is lower-is-better; we
    // negate to a positive score, then squash into (0,1] so it joins RRF cleanly.
    const char* sql =
        "SELECT doc_id, filename, doc_title, "
        "bm25(doc_fts5_index, 0.0, 1.0, 2.0, 1.5, 1.0) AS rank "
        "FROM doc_fts5_index WHERE doc_fts5_index MATCH ? "
        "ORDER BY rank LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        return DocSummaryStatus(DocSummaryErrorCode::kFts5FallbackFailed,
                                std::string("prepare search: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(st, 1, sanitized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, top_k);
    while (sqlite3_step(st) == SQLITE_ROW) {
        DocFtsHit h;
        const unsigned char* doc_id = sqlite3_column_text(st, 0);
        const unsigned char* fn = sqlite3_column_text(st, 1);
        const unsigned char* title = sqlite3_column_text(st, 2);
        h.doc_id = doc_id ? reinterpret_cast<const char*>(doc_id) : "";
        h.filename = fn ? reinterpret_cast<const char*>(fn) : "";
        h.doc_title = title ? reinterpret_cast<const char*>(title) : "";
        // bm25 rank is <= 0 (more negative = better match). Map to (0,1]:
        // score = 1/(1 + (-rank)). Best (rank≈0) → ~1.0; weak matches → → 0.
        double rank = sqlite3_column_double(st, 3);
        h.score = 1.0 / (1.0 + std::max(0.0, -rank));
        hits.push_back(std::move(h));
    }
    sqlite3_finalize(st);
    return hits;
}

std::vector<DocDiscoveryHit> FuseDocDiscovery(
    const std::vector<DocDiscoveryHit>& llm_summary_hits,
    const std::vector<DocFtsHit>& fts5_hits, int top_k, int k) {
    if (k <= 0) k = kDocDiscoveryRrfK;

    std::map<std::string, double> rrf;        // doc_id → fused score
    std::map<std::string, DocDiscoveryHit> meta;  // doc_id → richest metadata

    // llm_summary path (richer metadata wins on collision).
    for (size_t rank = 0; rank < llm_summary_hits.size(); ++rank) {
        const DocDiscoveryHit& h = llm_summary_hits[rank];
        rrf[h.doc_id] += 1.0 / (static_cast<double>(k) + static_cast<double>(rank));
        meta[h.doc_id] = h;  // llm_summary metadata
    }
    // fts5 fallback path — only supplies metadata for docs the summary path missed.
    for (size_t rank = 0; rank < fts5_hits.size(); ++rank) {
        const DocFtsHit& f = fts5_hits[rank];
        rrf[f.doc_id] += 1.0 / (static_cast<double>(k) + static_cast<double>(rank));
        if (meta.find(f.doc_id) == meta.end()) {
            DocDiscoveryHit h;
            h.doc_id = f.doc_id;
            h.via_path = "fts5_fallback";
            h.filename = f.filename;
            h.doc_title = f.doc_title;
            meta[f.doc_id] = h;
        }
    }

    std::vector<DocDiscoveryHit> out;
    out.reserve(meta.size());
    for (auto& [doc_id, h] : meta) {
        DocDiscoveryHit hit = h;
        hit.match_score = static_cast<float>(rrf[doc_id]);
        out.push_back(std::move(hit));
    }
    std::sort(out.begin(), out.end(), [](const DocDiscoveryHit& a, const DocDiscoveryHit& b) {
        return a.match_score > b.match_score;
    });
    if (top_k > 0 && static_cast<int>(out.size()) > top_k) {
        out.resize(top_k);
    }
    return out;
}

}  // namespace cortrix::doc_summary
