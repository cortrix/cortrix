#include "cortrix/query/single_unit_executor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>

#include "cortrix/query/cross_ns_error.h"

namespace cortrix::query {

using retrieval::NamespaceQueryResult;
using retrieval::RankedChunk;
using retrieval::ScoredResult;

SingleUnitExecutor::SingleUnitExecutor(INamespacePipeline* pipeline,
                                       reranker::IReranker* reranker,
                                       int candidate_multiplier,
                                       int max_candidates)
    : pipeline_(pipeline),
      reranker_(reranker),
      candidate_multiplier_(candidate_multiplier < 1 ? 1 : candidate_multiplier),
      max_candidates_(max_candidates < 1 ? 1 : max_candidates) {}

int SingleUnitExecutor::CandidateK(int top_k, float oversample) const {
    int k = top_k < 1 ? 1 : top_k;
    // Phase-1 oversample default is 1.0; values > 1.0 widen the over-fetch
    // (Phase-2 Unit-count-adaptive). ceil so a 1.5x oversample still over-fetches.
    int os = oversample > 1.0f ? static_cast<int>(std::ceil(oversample)) : 1;
    long long cand = static_cast<long long>(k) * candidate_multiplier_ * os;
    if (cand > max_candidates_) cand = max_candidates_;
    if (cand < k) cand = k;  // never fetch fewer than the caller's top_k
    return static_cast<int>(cand);
}

NamespaceQueryResult SingleUnitExecutor::ExecuteForNamespace(
    const QueryContext& ctx, const std::string& namespace_id, float oversample) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    NamespaceQueryResult out;
    out.namespace_id = namespace_id;
    out.retryable = false;
    out.retry_after_ms = 0;

    auto finish_latency = [&]() {
        out.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0)
                .count());
    };

    // A reranker is required unless the request opts out (RRF fallback). Missing
    // reranker on a rerank=true query is an internal misconfiguration for this NS.
    if (ctx.rerank && reranker_ == nullptr) {
        const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
        out.error_code = info.cx_code;
        out.error_category = agent_friendly::ToString(info.category);
        out.retryable = info.retryable;
        finish_latency();
        return out;
    }

    try {
        const int candidate_k = CandidateK(ctx.top_k, oversample);
        std::vector<ScoredResult> candidates =
            pipeline_->Retrieve(ctx, namespace_id, candidate_k);

        std::vector<RankedChunk> ranked;
        if (ctx.rerank) {
            // F02 frozen contract: Rerank loads chunk_text/parent_text via ChunkStore,
            // scores with the cross-encoder, and returns RankedChunks sorted by the
            // fused rerank score (RETRIEVAL_TYPES_SPEC §2 / reranker.h).
            ranked = reranker_->Rerank(candidates, ctx.query);
        } else {
            // RRF fallback (topic 3.2): no cross-encoder. We have child_id + RRF score
            // only; chunk_text/parent_text/metadata stay empty until F08/F09 wiring
            // (D3.5) — the gather layer flags meta.warnings["rerank_disabled"].
            ranked.reserve(candidates.size());
            for (const auto& c : candidates) {
                RankedChunk rc;
                rc.child_id = c.child_id;
                rc.score = c.score;          // RRF score carries the ordering
                rc.rerank_score = c.score;   // sort key == RRF score in fallback
                ranked.push_back(std::move(rc));
            }
            std::stable_sort(ranked.begin(), ranked.end(),
                             [](const RankedChunk& a, const RankedChunk& b) {
                                 return a.rerank_score > b.rerank_score;
                             });
        }

        // Truncate to the caller's top_k (over-fetch was only for reranker recall).
        const int top_k = ctx.top_k < 1 ? 1 : ctx.top_k;
        if (static_cast<int>(ranked.size()) > top_k) {
            ranked.resize(static_cast<std::size_t>(top_k));
        }

        out.chunks = std::move(ranked);
        out.error_code.clear();  // success
        finish_latency();
        return out;
    } catch (const std::exception&) {
        // Routine per-NS fault (index corrupt / resource) → in-band partial-success
        // error (topic 2.4), NOT a thrown exception. category=permanent, retryable=false.
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
