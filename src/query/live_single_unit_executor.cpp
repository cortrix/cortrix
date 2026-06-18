#include <cstdint>
#include "cortrix/query/live_single_unit_executor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

#include "cortrix/common/types.h"  // json alias (metadata_json flatten)
#include "cortrix/doc_summary/discover_handler.h"  // RecallDocSummaryHnsw (granularity=doc/both)
#include "cortrix/doc_summary/doc_summary_types.h"  // DocDiscoveryHit
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/scored_block.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/reranker/score_fusion.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/retrieval/sparse_codec.h"
#include "cortrix/retrieval/sparse_retriever.h"
#include "cortrix/retrieval/sparse_rrf.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/store/sqlite_chunk_store.h"

#include <unordered_map>

namespace cortrix::query {

using retrieval::NamespaceQueryResult;
using retrieval::RankedChunk;

namespace {

// Route-level deadline for the live two-route fan-out (the F04 per-NS timeout is
// enforced one level up by the ScatterGather join).
constexpr int64_t kRouteTimeoutUs = 5'000'000;  // 5s

}  // namespace

void FlattenMetadataIntoMap(const std::string& metadata_json,
                            std::map<std::string, std::string>& out) {
    if (metadata_json.empty()) return;
    try {
        json parsed = json::parse(metadata_json);
        if (!parsed.is_object()) return;  // only object-shaped metadata flattens
        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
            // Keep string values verbatim (so beir_corpus_id stays its raw id, not a
            // quoted "\"id\""); serialize any non-string scalar/array/object via dump()
            // so it survives as text in the string→string result map.
            out[it.key()] = it.value().is_string()
                                ? it.value().get<std::string>()
                                : it.value().dump();
        }
    } catch (...) {
        // Invalid metadata JSON: ignore, leave the map as-is (post_filter.cpp parity).
    }
}

LiveSingleUnitExecutor::LiveSingleUnitExecutor(cortrix::resource::INamespacePool& pool,
                                               cortrix::OnnxEmbedder& embedder,
                                               RRFFusion& fusion,
                                               reranker::IReranker* reranker,
                                               cortrix::retrieval::SparseIndexRegistry* sparse_registry,
                                               int candidate_multiplier,
                                               int max_candidates)
    : pool_(pool),
      embedder_(embedder),
      fusion_(fusion),
      reranker_(reranker),
      sparse_registry_(sparse_registry),
      candidate_multiplier_(candidate_multiplier < 1 ? 1 : candidate_multiplier),
      max_candidates_(max_candidates < 1 ? 1 : max_candidates) {}

int LiveSingleUnitExecutor::CandidateK(int top_k, float oversample) const {
    int k = top_k < 1 ? 1 : top_k;
    int os = oversample > 1.0f ? static_cast<int>(std::ceil(oversample)) : 1;
    long long cand = static_cast<long long>(k) * candidate_multiplier_ * os;
    if (cand > max_candidates_) cand = max_candidates_;
    if (cand < k) cand = k;
    return static_cast<int>(cand);
}

NamespaceQueryResult LiveSingleUnitExecutor::ExecuteForNamespace(
    const QueryContext& ctx, const std::string& namespace_id, float oversample) {
    // F41 §6.2 granularity dispatch. The default "auto" / "chunk" (and any unknown
    // value) take the existing chunk-level pipeline verbatim — the iron rule is that
    // the default path is 100% unchanged. Only "doc" / "both" engage the F41 §8.1
    // doc-summary branches. ScatterGather / the cross-NS gather are untouched.
    if (ctx.granularity == "doc") {
        return ExecuteDocRetrieval(ctx, namespace_id);
    }
    if (ctx.granularity == "both") {
        return ExecuteHybridRetrieval(ctx, namespace_id, oversample);
    }
    return ExecuteChunkRetrieval(ctx, namespace_id, oversample);
}

NamespaceQueryResult LiveSingleUnitExecutor::ExecuteChunkRetrieval(
    const QueryContext& ctx, const std::string& namespace_id, float oversample) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    NamespaceQueryResult out;
    out.namespace_id = namespace_id;

    auto finish_latency = [&]() {
        out.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0)
                .count());
    };

    // A reranker is required unless the request opts out (RRF fallback) — same
    // contract as SingleUnitExecutor (a rerank=true query with no reranker is a
    // per-NS misconfiguration → CX_ERR_INDEX_CORRUPT).
    if (ctx.rerank && reranker_ == nullptr) {
        const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
        out.error_code = info.cx_code;
        out.error_category = agent_friendly::ToString(info.category);
        out.retryable = info.retryable;
        finish_latency();
        return out;
    }

    try {
        // 1. Acquire the NS façade (F05 Pool.Acquire, RAII-released at scope exit).
        cortrix::resource::NamespaceFacade facade(pool_, namespace_id);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
            out.error_code = info.cx_code;
            out.error_category = agent_friendly::ToString(info.category);
            out.retryable = info.retryable;
            finish_latency();
            return out;
        }

        const int candidate_k = CandidateK(ctx.top_k, oversample);
        CortrixStore& store = facade.store();

        // 2. Run the per-NS recall routes. Vector + BM25 are the always-on dense +
        //    FTS5 paths; F40 sparse is added when a sparse registry is wired (else
        //    the fusion is just dense + FTS5). The retrieval-link boundary keys on
        //    child_id (ULID), so each route's block_id hits are mapped to child_id
        //    by reading the per-NS store; a legacy non-child row (empty child_id) is
        //    dropped. Blocks are cached here so the final RankedChunk assembly reuses
        //    them (one block_get per distinct child).
        VectorSearcher vec_searcher(facade.vec_index(), embedder_);
        BM25Searcher bm25_searcher(facade.store());
        RouteResult vector_result =
            vec_searcher.Search(ctx.query, candidate_k, kRouteTimeoutUs);
        RouteResult bm25_result =
            bm25_searcher.Search(ctx.query, candidate_k, kRouteTimeoutUs);

        // child_id → resolved chunk (text + optional metadata) cache, built as the
        // routes are converted. Dense/FTS5 resolve the full block (metadata_json);
        // a sparse-only child resolves text via the ChunkStore reverse-lookup.
        struct ChunkRow {
            std::string content_text;
            std::string metadata_json;
        };
        std::unordered_map<std::string, ChunkRow> by_child;

        // Convert a block_id-keyed RouteResult into a child_id-keyed ranked list
        // (RrfPath input), caching each resolved block. raw_score order is preserved
        // (the searchers already sort), so RRF rank = list position.
        auto to_child_hits = [&](const RouteResult& rr) {
            std::vector<retrieval::SparseHit> hits;
            if (!rr.ok()) return hits;
            hits.reserve(rr.items.size());
            for (const auto& it : rr.items) {
                CortrixBlock block;
                if (store.block_get(it.block_id, block) != 0) continue;
                if (block.child_id.empty()) continue;
                retrieval::SparseHit h;
                h.child_id = block.child_id;
                h.score = it.raw_score;
                by_child.emplace(block.child_id,
                                 ChunkRow{block.content_text, block.metadata_json});
                hits.push_back(std::move(h));
            }
            return hits;
        };

        retrieval::FivePathInput rrf_in;
        rrf_in.dense = to_child_hits(vector_result);
        rrf_in.fts5 = to_child_hits(bm25_result);

        // F40 sparse path (§6.3): embed the query's sparse vector and search the
        // per-NS SPLADE inverted index. A NS with no indexed sparse vectors (or a
        // missing registry / index open failure) yields an empty list → the fusion
        // degrades to dense+FTS5 (F40 §7.2 L2 fallback), no error.
        if (sparse_registry_ != nullptr) {
            retrieval::ISparseRetriever* sparse = sparse_registry_->GetOrOpen(namespace_id);
            if (sparse != nullptr && sparse->IsAvailable()) {
                EmbedWithSparseResult sp;
                if (embedder_.EmbedWithSparse(ctx.query, &sp).ok() && !sp.sparse.empty()) {
                    retrieval::SparseVector qvec;
                    qvec.terms = sp.sparse;
                    std::vector<retrieval::SparseHit> sparse_hits =
                        sparse->Search(qvec, namespace_id, candidate_k);
                    // Resolve text for sparse-only children (no dense/fts5 hit) via the
                    // ChunkStore reverse-lookup over this NS's blocks table.
                    cortrix::store::SqliteChunkStore chunk_store(store.db_handle());
                    for (const auto& h : sparse_hits) {
                        if (by_child.find(h.child_id) == by_child.end()) {
                            auto rec = chunk_store.Get(h.child_id);
                            if (!rec.ok()) continue;  // not resolvable → drop from fusion
                            by_child.emplace(h.child_id,
                                             ChunkRow{rec.value().content, std::string()});
                        }
                        rrf_in.sparse.push_back(h);
                    }
                }
            }
        }

        // chunk-level multi-path RRF (F40 §9.1). contextualized (F35) + hype (F38)
        // are fed empty this round per the design (mocked until those paths wire in).
        std::vector<retrieval::RrfFusedHit> fused =
            retrieval::FuseFivePathRrf(rrf_in, candidate_k);

        // 3. Assemble RankedChunks from the fused child_ids using the block cache.
        std::vector<RankedChunk> ranked;
        ranked.reserve(fused.size());
        for (const auto& fh : fused) {
            auto cit = by_child.find(fh.child_id);
            if (cit == by_child.end()) continue;
            const ChunkRow& row = cit->second;
            RankedChunk rc;
            rc.child_id = fh.child_id;
            rc.chunk_text = row.content_text;
            // parent_text reverse-lookup (F34 ParentChunkStore) is a separate seam;
            // left empty here per RETRIEVAL_TYPES_SPEC §1 ("empty until F34 wires in").
            // RankedChunk carries no parent_id field — ToResultItem sets ResultItem.
            // parent_id to its default until that F34 reverse-lookup lands (D3.5+).
            rc.score = fh.rrf_score;        // pre-rerank (multi-path RRF) score
            rc.rerank_score = fh.rrf_score;  // overwritten below when reranking
            // Flatten the block's metadata_json into TOP-LEVEL result-metadata keys
            // (e.g. beir_corpus_id), not a single opaque "metadata_json" blob — the
            // cross-NS runner matches qrels on those top-level keys (FiQA identity).
            FlattenMetadataIntoMap(row.metadata_json, rc.metadata);
            ranked.push_back(std::move(rc));
        }

        // 4. Rerank (shared ScoreBatch + F02 RerankerScoreFusion ordering) when the
        //    request asks for it; otherwise keep the RRF order (RRF fallback path).
        if (ctx.rerank && !ranked.empty()) {
            std::vector<const char*> passages;
            passages.reserve(ranked.size());
            for (const auto& rc : ranked) passages.push_back(rc.chunk_text.c_str());
            std::vector<float> scores =
                reranker_->ScoreBatch(ctx.query.c_str(), passages);
            for (std::size_t i = 0; i < ranked.size() && i < scores.size(); ++i) {
                ranked[i].rerank_score = scores[i];
            }
            // Fused ordering score = rerank*0.7 + rrf*0.3 (F02 §4.2-ter), local
            // sort-use only (not written into RankedChunk).
            const reranker::RerankerScoreFusion fusion;
            std::vector<std::pair<float, std::size_t>> order;
            order.reserve(ranked.size());
            for (std::size_t i = 0; i < ranked.size(); ++i) {
                const float f = fusion.ComputeRerankRrfScore(
                    ranked[i].rerank_score, ranked[i].score, ranked[i], ctx.query);
                order.emplace_back(f, i);
            }
            std::stable_sort(order.begin(), order.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<RankedChunk> sorted;
            sorted.reserve(ranked.size());
            for (const auto& [f, idx] : order) {
                (void)f;
                sorted.push_back(std::move(ranked[idx]));
            }
            ranked = std::move(sorted);
        } else {
            std::stable_sort(ranked.begin(), ranked.end(),
                             [](const RankedChunk& a, const RankedChunk& b) {
                                 return a.rerank_score > b.rerank_score;
                             });
        }

        // 5. Truncate to the caller's top_k (over-fetch was reranker recall only).
        const int top_k = ctx.top_k < 1 ? 1 : ctx.top_k;
        if (static_cast<int>(ranked.size()) > top_k) {
            ranked.resize(static_cast<std::size_t>(top_k));
        }

        out.chunks = std::move(ranked);
        out.error_code.clear();  // success
        finish_latency();
        return out;
    } catch (const std::exception&) {
        const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
        out.chunks.clear();
        out.error_code = info.cx_code;
        out.error_category = agent_friendly::ToString(info.category);
        out.retryable = info.retryable;
        finish_latency();
        return out;
    }
}

NamespaceQueryResult LiveSingleUnitExecutor::ExecuteDocRetrieval(
    const QueryContext& ctx, const std::string& namespace_id) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    NamespaceQueryResult out;
    out.namespace_id = namespace_id;
    auto finish_latency = [&]() {
        out.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0)
                .count());
    };

    try {
        // Acquire the NS façade (F05 Pool.Acquire, RAII-released at scope exit) — same
        // contract as the chunk path; a missing NS / store fault folds in-band as
        // CX_ERR_INDEX_CORRUPT (topic 2.4 partial success), never thrown.
        cortrix::resource::NamespaceFacade facade(pool_, namespace_id);
        if (!facade.Acquire().ok()) {
            const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
            out.error_code = info.cx_code;
            out.error_category = agent_friendly::ToString(info.category);
            out.retryable = info.retryable;
            finish_latency();
            return out;
        }

        const int top_k = ctx.top_k < 1 ? 1 : ctx.top_k;

        // §8.1 doc branch ≅ GET /documents/discover (main path): doc_summary embedding
        // HNSW recall over THIS NS's P-HNSW, via the shared doc_summary read core (one
        // recall implementation for both the endpoint and this path). The per-Unit
        // doc-level FTS5 fallback is the discover ENDPOINT's job (it owns the F08-field
        // index); the in-NS doc path surfaces the LLM-summary recall as doc-level units.
        std::vector<cortrix::doc_summary::DocDiscoveryHit> hits =
            cortrix::doc_summary::RecallDocSummaryHnsw(
                facade.vec_index(), facade.store(), embedder_, ctx.query, top_k);

        // Convert each doc-summary hit into a doc-level RankedChunk. child_id = doc_id
        // (the doc-level identity for this path; the cross-NS dedupe keys on content_hash
        // / child_id). chunk_text = summary_text so the §6.1 fields are available
        // downstream; metadata carries the B-class explain provenance (§8.3). No rerank:
        // doc summaries are not chunk passages, so HNSW similarity order is kept (the
        // §8.1 doc branch does not rerank doc-summary hits).
        out.chunks.reserve(hits.size());
        for (const auto& h : hits) {
            RankedChunk rc;
            rc.child_id = h.doc_id;             // doc-level identity for this path
            rc.chunk_text = h.summary_text;     // §4.2 summary_text
            rc.score = h.match_score;           // HNSW similarity (pre-rerank)
            rc.rerank_score = h.match_score;    // no rerank on the doc path → mirror score
            rc.metadata["via_path"] = "doc_summary";  // §8.3 B-class provenance
            rc.metadata["source_doc_id"] = h.doc_id;
            rc.metadata["doc_summary_match_score"] = std::to_string(h.match_score);
            if (!h.one_liner.empty()) rc.metadata["one_liner"] = h.one_liner;
            out.chunks.push_back(std::move(rc));
        }

        out.error_code.clear();  // success (an empty doc-summary set is still success)
        finish_latency();
        return out;
    } catch (const std::exception&) {
        const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
        out.chunks.clear();
        out.error_code = info.cx_code;
        out.error_category = agent_friendly::ToString(info.category);
        out.retryable = info.retryable;
        finish_latency();
        return out;
    }
}

NamespaceQueryResult LiveSingleUnitExecutor::ExecuteHybridRetrieval(
    const QueryContext& ctx, const std::string& namespace_id, float oversample) {
    // §8.1 both branch: run BOTH paths and concatenate (chunk units first, then
    // doc-level). The cross-NS gather re-sorts by rerank_score and dedupes, so keeping
    // both kinds in the candidate set is correct; the per-NS top_k cap is applied here
    // so a NS does not over-contribute. A per-path NS failure (error_code set) is
    // surfaced if BOTH fail; if only one fails we still return the other's results
    // (partial success — the chunk path is the primary).
    NamespaceQueryResult chunk_part = ExecuteChunkRetrieval(ctx, namespace_id, oversample);
    NamespaceQueryResult doc_part = ExecuteDocRetrieval(ctx, namespace_id);

    NamespaceQueryResult out;
    out.namespace_id = namespace_id;
    // Latency = the larger of the two (they run sequentially here; the sum would
    // double-count the shared façade acquire — max is the honest single-NS wall time
    // floor, and this path is not on the default hot path).
    out.latency_ms = std::max(chunk_part.latency_ms, doc_part.latency_ms);

    const bool chunk_ok = chunk_part.error_code.empty();
    const bool doc_ok = doc_part.error_code.empty();
    if (!chunk_ok && !doc_ok) {
        // Both failed → surface the chunk path's error (the primary path).
        out.error_code = chunk_part.error_code;
        out.error_category = chunk_part.error_category;
        out.retryable = chunk_part.retryable;
        out.retry_after_ms = chunk_part.retry_after_ms;
        return out;
    }

    if (chunk_ok) {
        for (auto& rc : chunk_part.chunks) out.chunks.push_back(std::move(rc));
    }
    if (doc_ok) {
        for (auto& rc : doc_part.chunks) out.chunks.push_back(std::move(rc));
    }
    const int top_k = ctx.top_k < 1 ? 1 : ctx.top_k;
    if (static_cast<int>(out.chunks.size()) > top_k) {
        out.chunks.resize(static_cast<std::size_t>(top_k));
    }
    out.error_code.clear();  // success (at least one path succeeded)
    return out;
}

}  // namespace cortrix::query
