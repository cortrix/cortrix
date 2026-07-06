#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/doc_summary/doc_summary_types.h"  // DocDiscoveryHit

// httplib + RequestContext are only needed by the thin HTTP wrapper; forward-declare
// to keep the pure ExecuteDocDiscovery / RecallDocSummaryHnsw core free of the server
// build (the route registration itself lives in bootstrap, not here).
namespace httplib { struct Request; struct Response; }

namespace cortrix {
class OnnxEmbedder;
class CortrixStore;
struct RequestContext;
class ApiKeyAuth;
namespace store { class IIndex; }
namespace resource { class INamespacePool; }
}  // namespace cortrix

namespace cortrix::doc_summary {

// Resolved F41 config (defined in doc_summary_generator.h; SoT keys/defaults in
// doc_summary_config.h). The endpoint takes it by const-ref only, so a forward
// declaration keeps this header light — callers that pass an instance include the
// generator header; discover_handler.cpp includes it for the member access.
struct DocSummaryConfig;

/// doc-summary HNSW recall (F41 §6.1 Step 1 / §8.2 main path). Embeds `query`, runs
/// a single ANN search over the per-NS P-HNSW index (the SAME mixed pool the chunk
/// path uses — doc_summary blocks were AddPoints'd by F41AsyncWorker), then resolves
/// each hit from `store` and keeps ONLY block_type==kBlockDocSummary(17) rows whose
/// metadata_json.status == "generated" (a failed/pending doc has no such block, so it
/// is naturally absent → the §7.1 FTS5 fallback covers it). The 4 structured fields
/// (summary_text=content_text + keywords/topics/one_liner from metadata_json) fill the
/// DocDiscoveryHit; via_path = "llm_summary". `score` is the HNSW L2→similarity score
/// (1/(1+distance)), used only for ordering (RRF rank). Results are sorted DESC and
/// truncated to `top_k`. A NS with no doc_summary blocks returns an empty list (not an
/// error). This is the shared core consumed BOTH by ExecuteDocDiscovery (the endpoint)
/// and by LiveSingleUnitExecutor's granularity=doc/both path (one recall implementation).
std::vector<DocDiscoveryHit> RecallDocSummaryHnsw(store::IIndex& index,
                                                  cortrix::CortrixStore& store,
                                                  cortrix::OnnxEmbedder& embedder,
                                                  const std::string& query, int top_k);

/// Shared doc-discovery core used by BOTH the HTTP discover endpoint and query
/// granularity=doc/both. It runs:
///   1) doc_summary embedding HNSW recall ("llm_summary");
///   2) optional doc-level FTS5 fallback over F08 fields ("fts5_fallback");
///   3) FuseDocDiscovery RRF over doc_id.
/// The core is intentionally below the NamespaceFacade boundary so callers that
/// already acquired a per-NS facade can reuse the exact same candidate-generation
/// logic without reacquiring the namespace.
struct DocDiscoveryCoreOptions {
    int top_k = 10;
    bool fts5_fallback_enabled = true;
};

struct DocDiscoveryCoreResult {
    std::vector<DocDiscoveryHit> hits;
    bool hnsw_ran = false;
    bool hnsw_failed = false;
    bool fts5_ran = false;
    bool fts5_failed = false;
    int summary_hit_count = 0;
    int fts5_hit_count = 0;
    std::vector<std::string> warnings;
};

DocDiscoveryCoreResult RunDocDiscoveryCore(store::IIndex& index,
                                           cortrix::CortrixStore& store,
                                           cortrix::OnnxEmbedder& embedder,
                                           const std::string& query,
                                           const DocDiscoveryCoreOptions& options);

/// Pure doc-discovery executor (F41 §6.1 / §8.2 — test-friendly, no httplib). Acquires
/// the NS facade from `pool`, runs the two hybrid recall paths and RRF-fuses them:
///   Step 1 (main):     RecallDocSummaryHnsw (doc_summary embedding HNSW) → "llm_summary"
///   Step 2 (fallback): a per-NS doc-level FTS5 index over the F08 fields → "fts5_fallback"
///                      (config.fts5_fallback_enabled gate; a query-time FTS5 failure is a
///                      graceful degrade — increments cortrix_fts5_fallback_failed_total and
///                      returns the main-path results only, never throws, F41-8/§8.2).
///   Step 3:            FuseDocDiscovery (RRF, dedup by doc_id) → top_k.
/// Returns the §6.1 response body { results:[...], meta:{succeeded,failed,coverage_ratio,
/// warnings} }. `explain` adds the C-class meta.fallback_triggered signal. A NS that
/// cannot be acquired yields coverage_ratio 0 + the NS in meta.failed (partial-success
/// shape, never an exception).
nlohmann::json ExecuteDocDiscovery(cortrix::resource::INamespacePool& pool,
                                   cortrix::OnnxEmbedder& embedder,
                                   const std::string& ns, const std::string& query,
                                   int top_k, bool explain,
                                   const DocSummaryConfig& config);

/// Thin httplib wrapper around ExecuteDocDiscovery for GET /api/v1/documents/discover
/// (F41 §6.1). Reads ?query (required) / ?top_k (default 10, clamped 1..100) / ?ns or
/// ?namespace (required) / ?explain (truthy "true"/"1"); writes the JSON body (200) or
/// the Agent-friendly InvalidArgument/NotFound error envelope. Does NOT register itself
/// on a server — bootstrap mounts it (BEFORE the GET /documents/{id} catch-all so the
/// literal "discover" segment is not swallowed). The caller passes the resolved
/// DocSummaryConfig (bootstrap resolves it once from IGlobalConfig).
void HandleDocumentsDiscover(const httplib::Request& req, httplib::Response& res,
                             const cortrix::RequestContext& rctx,
                             cortrix::resource::INamespacePool& pool,
                             cortrix::OnnxEmbedder& embedder,
                             const DocSummaryConfig& config);

}  // namespace cortrix::doc_summary
