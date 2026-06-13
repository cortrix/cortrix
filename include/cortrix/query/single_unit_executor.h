#pragma once
#include <string>

#include "cortrix/query/i_namespace_pipeline.h"
#include "cortrix/query/i_scatter_executor.h"
#include "cortrix/query/query_context.h"
#include "cortrix/reranker.h"            // cortrix::reranker::IReranker (F02 frozen header)
#include "cortrix/retrieval/cross_ns_types.h"  // NamespaceQueryResult

namespace cortrix::query {

/// SingleUnitExecutor — V1's only IScatterExecutor impl (F04 §2.1 / S1.2).
/// 1 NS = 1 Unit, so it runs the NS's whole pipeline in-line:
///   Retrieve (Vector+BM25 → RRF, via INamespacePipeline)  →
///   IReranker::Rerank (F02 frozen contract; skipped when ctx.rerank == false)  →
///   truncate to top_k  →  NamespaceQueryResult.
///
/// 🚨 D3 standalone: it programs against the INamespacePipeline + IReranker
/// *interfaces* and never touches live MVP instances. Wiring the real per-NS
/// pipeline (VectorSearcher/BM25Searcher/RRFFusion bound to a live index/store)
/// and the real OnnxReranker is **D3.5**. Unit tests inject a mock pipeline + the
/// shared MockReranker (F02 §7.2).
///
/// candidate over-fetch: candidate_k = min(top_k × multiplier, max_candidates)
/// (F02 §top_N — multiplier/cap mirror RerankerConfig defaults 3 / 50). The
/// reranker reorders by cross-encoder score (comparable across NS, ARCH §3.3); when
/// rerank is off, the RRF score carries the ordering (RRF fallback — final gather
/// fallback lives in ScatterGather, this NS-local path just preserves RRF order).
class SingleUnitExecutor : public IScatterExecutor {
public:
    /// @param pipeline  the Retrieve stage (NOT owned).
    /// @param reranker  F02 reranker (NOT owned); may be null only if every query
    ///                 sets rerank=false (otherwise the NS reports CX_ERR_INDEX_CORRUPT).
    /// @param candidate_multiplier / max_candidates  over-fetch sizing (F02 §top_N).
    SingleUnitExecutor(INamespacePipeline* pipeline,
                       reranker::IReranker* reranker,
                       int candidate_multiplier = 3,
                       int max_candidates = 50);

    retrieval::NamespaceQueryResult ExecuteForNamespace(
        const QueryContext& ctx,
        const std::string& namespace_id,
        float oversample = 1.0f) override;

    /// candidate_k = min(top_k × multiplier × ceil(oversample), max_candidates),
    /// clamped to >= top_k and >= 1. Exposed for unit testing the sizing rule.
    int CandidateK(int top_k, float oversample) const;

private:
    INamespacePipeline* pipeline_;
    reranker::IReranker* reranker_;
    int candidate_multiplier_;
    int max_candidates_;
};

}  // namespace cortrix::query
