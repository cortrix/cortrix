#pragma once
#include <string>

#include "cortrix/retrieval/types.h"

namespace cortrix::reranker {

/// RerankerScoreFusion — reranker-owned RRF score fusion.
///
/// Fuses the cross-encoder precision score (`rerank_score`) with the multi-
/// recall RRF score (`rrf_score`) into a single ordering score that
/// `OnnxReranker::Rerank` sorts on. This is **the reranker's own**
/// fusion — distinct from `SemanticScorer::ComputeFinalScore` (α-multiplier
/// fusion of enriched+semantic scores), which is a separate
/// operation the reranker neither redefines nor impersonates.
///
/// The fused score is the final ordering score. Callers that return
/// RankedChunk objects must write it back to RankedChunk::score so downstream
/// scatter, dedup, response serialization, and replay surfaces agree on the same
/// final score.
///
/// V1 simplified: weighted_sum(rerank_score * 0.7 + rrf_score * 0.3), returning a
/// normalized [0,1] ordering score. Phase 2 extends with business-hotness signals
/// (Block hotness self-learning) + recency signals.
class RerankerScoreFusion {
public:
    /// V1 fusion weights. Kept as named constants so the
    /// 0.7/0.3 split is a single source of truth (and inspectable in tests).
    static constexpr float kRerankWeight = 0.7f;  ///< cross-encoder precision weight
    static constexpr float kRrfWeight    = 0.3f;  ///< multi-recall RRF weight

    /// Fuse the cross-encoder score with the RRF score into the final ordering
    /// score (normalized [0,1] when both inputs are in [0,1]).
    ///
    /// @param rerank_score  cross-encoder score (ScoreBatch output, [0,1])
    /// @param rrf_score     RRF score (ScoredResult.score / RankedChunk.score)
    /// @param chunk         the candidate (Phase 2 hotness/recency hook; unused in V1)
    /// @param query         the query (Phase 2 hook; unused in V1)
    /// @return              final ordering score
    float ComputeRerankRrfScore(float rerank_score, float rrf_score,
                                const cortrix::retrieval::RankedChunk& chunk,
                                const std::string& query) const;
};

}  // namespace cortrix::reranker
