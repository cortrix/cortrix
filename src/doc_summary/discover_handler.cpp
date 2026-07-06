#include "cortrix/doc_summary/discover_handler.h"

#include <algorithm>

#include "cortrix/common/block_types.h"          // kBlockDocSummary
#include "cortrix/common/data_types.h"            // CortrixBlock
#include "cortrix/doc_summary/doc_fts5_index.h"   // SearchDocFts5 / FuseDocDiscovery
#include "cortrix/doc_summary/doc_summary_config.h"     // kDocDiscoveryRrfK
#include "cortrix/doc_summary/doc_summary_generator.h"  // DocSummaryConfig (full def for config.fts5_fallback_enabled)
#include "cortrix/doc_summary/doc_summary_metrics.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/http_server.h"           // WriteJsonError / WriteJsonResponse
#include "cortrix/server/request_context.h"
#include "cortrix/spc/onnx_embedder.h"            // OnnxEmbedder / EmbeddingResult
#include "cortrix/store/cortrix_store.h"           // CortrixStore
#include "cortrix/store/iindex.h"                  // IIndex

#include "httplib.h"

namespace cortrix::doc_summary {

namespace {

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

DocDiscoveryCoreResult RunDocDiscoveryCore(store::IIndex& index,
                                           cortrix::CortrixStore& store,
                                           cortrix::OnnxEmbedder& embedder,
                                           const std::string& query,
                                           const DocDiscoveryCoreOptions& options) {
    DocDiscoveryCoreResult result;
    const int k = options.top_k < 1 ? 1 : options.top_k;

    // Step 1 (main): doc_summary embedding HNSW recall. A query-time HNSW/index/embed
    // fault is a graceful degrade: record a warning and let the doc-level FTS5 fallback
    // still generate doc candidates. This is the same partial-success behavior used by
    // the HTTP discover endpoint and now by the query granularity=doc/both path.
    std::vector<DocDiscoveryHit> llm_hits;
    result.hnsw_ran = true;
    try {
        llm_hits = RecallDocSummaryHnsw(index, store, embedder, query, k);
        result.summary_hit_count = static_cast<int>(llm_hits.size());
    } catch (const std::exception& e) {
        result.hnsw_failed = true;
        result.warnings.push_back(std::string("doc_summary_hnsw_failed: ") + e.what());
    }

    // Step 2 (fallback): per-Unit doc-level FTS5 over the F08 fields. Query-time
    // FTS5 faults are graceful degrade events, counted by the existing F41 metric.
    std::vector<DocFtsHit> fts5_hits;
    if (options.fts5_fallback_enabled) {
        result.fts5_ran = true;
        auto fts5 = SearchDocFts5(store.db_handle(), query, k);
        if (fts5.ok()) {
            fts5_hits = std::move(fts5.value());
            result.fts5_hit_count = static_cast<int>(fts5_hits.size());
        } else {
            result.fts5_failed = true;
            DocSummaryMetrics::Instance().RecordFts5FallbackFailed();
            result.warnings.push_back(
                std::string("fts5_fallback_failed: ") + fts5.status().message());
        }
    }

    // Step 3: RRF-fuse the two doc-level recall paths by doc_id.
    result.hits = FuseDocDiscovery(llm_hits, fts5_hits, k, kDocDiscoveryRrfK);
    return result;
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

    DocDiscoveryCoreOptions core_opts;
    core_opts.top_k = k;
    core_opts.fts5_fallback_enabled = config.fts5_fallback_enabled;
    DocDiscoveryCoreResult core =
        RunDocDiscoveryCore(facade.vec_index(), facade.store(), embedder, query, core_opts);
    for (const auto& warning : core.warnings) warnings.push_back(warning);

    const std::vector<DocDiscoveryHit>& fused = core.hits;
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
        const bool triggered = core.fts5_failed || fallback_docs > 0;
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
