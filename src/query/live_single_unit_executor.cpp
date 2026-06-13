#include "cortrix/query/live_single_unit_executor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

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
            if (!row.metadata_json.empty()) {
                rc.metadata["metadata_json"] = row.metadata_json;
            }
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

}  // namespace cortrix::query
