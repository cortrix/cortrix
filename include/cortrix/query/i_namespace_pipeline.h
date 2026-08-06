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
/// 🚨 standalone: SingleUnitExecutor is written against THIS interface, not
/// against the concrete MVP classes (VectorSearcher / BM25Searcher / RRFFusion,
/// which are bound to live per-NS CortrixVectorIndex / CortrixStore instances).
/// Adapting this interface onto the real MVP pipeline (and wiring per-NS index/
/// store handles) is **integration** — flagged, not done here. Unit tests
/// drive a mock of this interface; the CRAG and RAG-Fusion stages can also reuse it.
///
/// Output is `retrieval::ScoredResult[]` (child_id + fused score) — exactly the
/// candidate type `IReranker::Rerank` consumes.
class INamespacePipeline {
public:
    virtual ~INamespacePipeline() = default;

    /// Run Vector+BM25 → RRF for one NS and return the fused candidates.
    /// @param ctx           shared request context (query / filter / rerank flag).
    /// @param namespace_id  the NS being queried.
    /// @param candidate_k   number of fused candidates to return (the reranker
    ///                      over-fetch count: top_k × multiplier, capped).
    /// Throwing is reserved for genuine internal faults; routine "NS index corrupt" is
    /// surfaced by SingleUnitExecutor as an in-band NamespaceQueryResult error.
    virtual std::vector<retrieval::ScoredResult> Retrieve(
        const QueryContext& ctx,
        const std::string& namespace_id,
        int candidate_k) = 0;
};

}  // namespace cortrix::query
