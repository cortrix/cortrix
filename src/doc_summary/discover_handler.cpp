#include "cortrix/doc_summary/discover_handler.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>

#include "cortrix/common/block_types.h"          // kBlockDocSummary
#include "cortrix/common/data_types.h"            // CortrixBlock
#include "cortrix/doc_summary/doc_fts5_index.h"   // DocFtsHit / FuseDocDiscovery
#include "cortrix/doc_summary/doc_summary_config.h"     // kDocDiscoveryRrfK
#include "cortrix/doc_summary/doc_summary_generator.h"  // DocSummaryConfig (full def for config.fts5_fallback_enabled)
#include "cortrix/doc_summary/doc_summary_metrics.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/http_server.h"           // WriteJsonError / WriteJsonResponse
#include "cortrix/server/request_context.h"
#include "cortrix/spc/onnx_embedder.h"            // OnnxEmbedder / EmbeddingResult
#include "cortrix/store/cortrix_store.h"           // CortrixStore
#include "cortrix/store/cortrix_store_sqlite.h"    // SanitizeFts5Query (M-SEC-001)
#include "cortrix/store/iindex.h"                  // IIndex

#include "httplib.h"

namespace cortrix::doc_summary {

namespace {

// §8.2 doc-level BM25 query against a per-Unit `doc_fts5_index` table reached through
// an existing store handle (the F41SchemaProvider creates this table per Unit;
// populating it is the F08-rev-N write hook = D3.5, so live it is empty until that
// lands — an empty result here just means the main HNSW path stands alone). The query,
// column weights (filename 1.0 / doc_title 2.0 / topics 1.5 / authors 1.0), and the
// bm25→(0,1] score map MIRROR DocFts5Index::Search exactly; we read the per-Unit table
// directly rather than via DocFts5Index because that class owns its own DB connection.
// Throws std::runtime_error on a SQLite fault so the caller can graceful-degrade (§8.2).
std::vector<DocFtsHit> SearchDocFts5OverHandle(sqlite3* db, const std::string& query,
                                               int top_k) {
    std::vector<DocFtsHit> hits;
    if (db == nullptr || top_k <= 0) return hits;
    const std::string sanitized = cortrix::SanitizeFts5Query(query);
    if (sanitized.empty()) return hits;  // empty / operator-only query → no rows (not an error)

    const char* sql =
        "SELECT doc_id, filename, doc_title, "
        "bm25(doc_fts5_index, 0.0, 1.0, 2.0, 1.5, 1.0) AS rank "
        "FROM doc_fts5_index WHERE doc_fts5_index MATCH ? "
        "ORDER BY rank LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("doc_fts5 prepare: ") + sqlite3_errmsg(db));
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
        const double rank = sqlite3_column_double(st, 3);
        h.score = 1.0 / (1.0 + std::max(0.0, -rank));
        hits.push_back(std::move(h));
    }
    sqlite3_finalize(st);
    return hits;
}

// Serialize one fused DocDiscoveryHit into the §6.1 result object. The llm_summary
// path carries the 4 structured fields; the fts5_fallback path carries the F08 fields.
nlohmann::json HitToJson(const DocDiscoveryHit& h) {
    nlohmann::json j;
    j["doc_id"] = h.doc_id;
    j["via_path"] = h.via_path;
    j["match_score"] = h.match_score;
    if (h.via_path == "llm_summary") {
        j["summary_text"] = h.summary_text;
        j["keywords"] = h.keywords;
        j["topics"] = h.topics;
        j["one_liner"] = h.one_liner;
    } else {  // "fts5_fallback"
        j["doc_title"] = h.doc_title;
        j["filename"] = h.filename;
    }
    return j;
}

}  // namespace

std::vector<DocDiscoveryHit> RecallDocSummaryHnsw(store::IIndex& index,
                                                  cortrix::CortrixStore& store,
                                                  cortrix::OnnxEmbedder& embedder,
                                                  const std::string& query, int top_k) {
    std::vector<DocDiscoveryHit> out;
    if (top_k <= 0) return out;

    // Embed the query (dense). On an embed failure the doc-summary main path simply
    // yields nothing (the endpoint's FTS5 fallback / partial-success meta cover it).
    EmbeddingResult emb;
    if (!embedder.Embed(query, &emb).ok() || emb.vector.empty()) return out;

    // One ANN search over the SAME mixed P-HNSW pool the chunk path uses. doc_summary
    // blocks were AddPoints'd by F41AsyncWorker; we over-fetch (×4) because the pool is
    // shared with chunk/hype blocks and we filter to block_type==17 below.
    const int search_k = top_k * 4;
    auto raw = index.Search(emb.vector.data(), search_k);

    for (const auto& [block_id, distance] : raw) {
        CortrixBlock block;
        if (store.block_get(block_id, block) != 0) continue;
        if (block.block_type != kBlockDocSummary) continue;  // only doc_summary blocks
        // Parse the §4.2 metadata_json column (keywords/topics/one_liner/status). A
        // failed/pending doc has no doc_summary block, so a present block is "generated";
        // we still gate on status defensively (skip anything not "generated").
        DocDiscoveryHit hit;
        hit.doc_id = block.doc_id;
        hit.via_path = "llm_summary";
        hit.summary_text = block.content_text;  // §4.2: summary_text rides on content_text
        hit.match_score = 1.0f / (1.0f + distance);  // L2 distance → similarity (ordering only)
        if (!block.metadata_json.empty()) {
            try {
                const nlohmann::json m = nlohmann::json::parse(block.metadata_json);
                if (m.value("status", "generated") != "generated") continue;
                if (m.contains("keywords") && m["keywords"].is_array())
                    hit.keywords = m["keywords"].get<std::vector<std::string>>();
                if (m.contains("topics") && m["topics"].is_array())
                    hit.topics = m["topics"].get<std::vector<std::string>>();
                hit.one_liner = m.value("one_liner", "");
            } catch (...) {
                // Unparseable metadata → keep the summary_text + doc_id (still useful).
            }
        }
        out.push_back(std::move(hit));
        if (static_cast<int>(out.size()) >= top_k) break;
    }

    // Already in ascending-distance (descending-similarity) order from the index, but
    // sort defensively by match_score DESC so the RRF rank is well-defined.
    std::sort(out.begin(), out.end(),
              [](const DocDiscoveryHit& a, const DocDiscoveryHit& b) {
                  return a.match_score > b.match_score;
              });
    return out;
}

nlohmann::json ExecuteDocDiscovery(cortrix::resource::INamespacePool& pool,
                                   cortrix::OnnxEmbedder& embedder,
                                   const std::string& ns, const std::string& query,
                                   int top_k, bool explain,
                                   const DocSummaryConfig& config) {
    nlohmann::json out;
    nlohmann::json results = nlohmann::json::array();
    nlohmann::json meta;
    nlohmann::json succeeded = nlohmann::json::array();
    nlohmann::json failed = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();

    const int k = top_k < 1 ? 1 : top_k;

    // Acquire the NS façade (RAII released at scope exit). A failure is partial-success
    // shape: coverage_ratio 0 + the NS in meta.failed, never an exception (GEN-Agent #3).
    cortrix::resource::NamespaceFacade facade(pool, ns);
    if (!facade.Acquire().ok()) {
        failed.push_back(ns);
        meta["succeeded"] = succeeded;
        meta["failed"] = failed;
        meta["coverage_ratio"] = 0.0;
        meta["warnings"] = warnings;
        out["results"] = results;
        out["meta"] = meta;
        return out;
    }

    // Step 1 (main): doc_summary embedding HNSW recall (via_path = "llm_summary").
    // A query-time HNSW/index/embed fault is a graceful degrade (GEN-Agent #3,
    // mirroring the fts5 fallback below + the §161 partial-success contract):
    // record a warning and fall back to the fts5 path only — never let it escape
    // as a generic 500.
    std::vector<DocDiscoveryHit> llm_hits;
    try {
        llm_hits =
            RecallDocSummaryHnsw(facade.vec_index(), facade.store(), embedder, query, k);
    } catch (const std::exception& e) {
        warnings.push_back(std::string("doc_summary_hnsw_failed: ") + e.what());
    }

    // Step 2 (fallback): per-Unit doc-level FTS5 over the F08 fields (via_path =
    // "fts5_fallback"). Gated by config.fts5_fallback_enabled (F41 §4.4). A query-time
    // FTS5 fault is a graceful degrade (F41-8 / §8.2): record the metric + a warning,
    // and return the main-path results only — never fail the request.
    std::vector<DocFtsHit> fts5_hits;
    bool fts5_failed = false;
    if (config.fts5_fallback_enabled) {
        try {
            fts5_hits = SearchDocFts5OverHandle(facade.store().db_handle(), query, k);
        } catch (const std::exception& e) {
            fts5_failed = true;
            DocSummaryMetrics::Instance().RecordFts5FallbackFailed();
            warnings.push_back(std::string("fts5_fallback_failed: ") + e.what());
        }
    }

    // Step 3: RRF-fuse the two paths (dedup by doc_id) → top_k (FuseDocDiscovery SoT).
    // k = kDocDiscoveryRrfK (60, §8.2 industry default; NS-configurable is Phase 2).
    std::vector<DocDiscoveryHit> fused =
        FuseDocDiscovery(llm_hits, fts5_hits, k, kDocDiscoveryRrfK);
    for (const auto& h : fused) results.push_back(HitToJson(h));

    succeeded.push_back(ns);
    meta["succeeded"] = succeeded;
    meta["failed"] = failed;
    meta["coverage_ratio"] = 1.0;
    meta["warnings"] = warnings;
    if (explain) {
        // C-class debug signal (§8.3): surface only when the fallback path actually
        // contributed or faulted (i.e. it "ran"). fallback_docs_count = #docs that came
        // in via the fts5 path (present in fused with via_path == "fts5_fallback").
        int fallback_docs = 0;
        for (const auto& h : fused)
            if (h.via_path == "fts5_fallback") ++fallback_docs;
        const bool triggered = fts5_failed || fallback_docs > 0;
        if (triggered) {
            meta["fallback_triggered"] = true;
            meta["fallback_docs_count"] = fallback_docs;
        }
    }
    out["results"] = results;
    out["meta"] = meta;
    return out;
}

void HandleDocumentsDiscover(const httplib::Request& req, httplib::Response& res,
                             const cortrix::RequestContext& rctx,
                             cortrix::resource::INamespacePool& pool,
                             cortrix::OnnxEmbedder& embedder,
                             const DocSummaryConfig& config) {
    // ?query (required)
    if (!req.has_param("query")) {
        cortrix::WriteJsonError(res, Status::InvalidArgument("query is required"),
                                rctx.request_id);
        return;
    }
    const std::string query = req.get_param_value("query");
    if (query.empty()) {
        cortrix::WriteJsonError(res, Status::InvalidArgument("query must not be empty"),
                                rctx.request_id);
        return;
    }

    // ?ns or ?namespace (required) — accept both spellings (§6.1 uses `ns`; the rest of
    // the documents surface uses `namespace`).
    std::string ns;
    if (req.has_param("ns")) ns = req.get_param_value("ns");
    else if (req.has_param("namespace")) ns = req.get_param_value("namespace");
    if (ns.empty()) {
        cortrix::WriteJsonError(res, Status::InvalidArgument("ns (namespace) is required"),
                                rctx.request_id);
        return;
    }

    // ?top_k (default 10, clamped 1..100 — mirrors the query top_k bounds)
    int top_k = 10;
    if (req.has_param("top_k")) {
        try {
            top_k = std::stoi(req.get_param_value("top_k"));
        } catch (...) {
            cortrix::WriteJsonError(res, Status::InvalidArgument("top_k must be an integer"),
                                    rctx.request_id);
            return;
        }
        if (top_k < 1 || top_k > 100) {
            cortrix::WriteJsonError(res, Status::InvalidArgument("top_k must be between 1 and 100"),
                                    rctx.request_id);
            return;
        }
    }

    // ?explain (truthy "true"/"1") — same convention as the query path.
    bool explain = false;
    if (req.has_param("explain")) {
        const std::string v = req.get_param_value("explain");
        explain = (v == "true" || v == "TRUE" || v == "True" || v == "1");
    }

    nlohmann::json body =
        ExecuteDocDiscovery(pool, embedder, ns, query, top_k, explain, config);
    cortrix::WriteJsonResponse(res, 200, body, rctx.request_id);
}

}  // namespace cortrix::doc_summary
