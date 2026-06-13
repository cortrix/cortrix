#pragma once
#include <string>
#include <vector>

#include "cortrix/query/query_context.h"
#include "cortrix/retrieval/types.h"  // cortrix::retrieval::ScoredResult

namespace cortrix::query {

/// INamespacePipeline — the narrow contract for "the existing V1 pipeline inside one NS up
/// to the fused-candidate stage" (Vector+BM25 → RRF), i.e. everything
/// SingleUnitExecutor needs *before* the reranker step.
///
/// 🚨 D3 standalone: SingleUnitExecutor is written against THIS interface, not
/// against the concrete MVP classes (VectorSearcher / BM25Searcher / RRFFusion,
/// which are bound to live per-NS CortrixVectorIndex / CortrixStore instances).
/// Adapting this interface onto the real MVP pipeline (and wiring per-NS index/
/// store handles) is **D3.5 integration** — flagged, not done here. Unit tests
/// drive a mock of this interface; F37/F36 can also reuse it.
///
/// Output is `retrieval::ScoredResult[]` (child_id + fused score) — exactly the
/// candidate type F02 `IReranker::Rerank` consumes (RETRIEVAL_TYPES_SPEC §2).
class INamespacePipeline {
public:
    virtual ~INamespacePipeline() = default;

    /// Run Vector+BM25 → RRF for one NS and return the fused candidates.
    /// @param ctx           shared request context (query / filter / rerank flag).
    /// @param namespace_id  the NS being queried.
    /// @param candidate_k   number of fused candidates to return (the reranker
    ///                      over-fetch count: top_k × multiplier, capped — F02 §top_N).
    /// Throwing is reserved for genuine internal faults; routine "NS index corrupt" is
    /// surfaced by SingleUnitExecutor as an in-band NamespaceQueryResult error.
    virtual std::vector<retrieval::ScoredResult> Retrieve(
        const QueryContext& ctx,
        const std::string& namespace_id,
        int candidate_k) = 0;
};

}  // namespace cortrix::query
