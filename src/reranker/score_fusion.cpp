#include "cortrix/reranker/score_fusion.h"

#include "cortrix/scoring/semantic_scorer.h"

namespace cortrix::reranker {

float RerankerScoreFusion::ComputeRerankRrfScore(
    float rerank_score, float rrf_score,
    const cortrix::retrieval::RankedChunk& chunk,
    const std::string& query) const {
    // F02 base fusion: weighted_sum of the cross-encoder precision score and the
    // F36 RRF recall score. When F03/F07 score signals are present, delegate the
    // semantic multiplier to F07's existing scoring policy.
    const float base_score = rerank_score * kRerankWeight + rrf_score * kRrfWeight;
    if (chunk.score_signals.HasAny()) {
        return cortrix::scoring::SemanticScorer::ComputeFinalScore(
            base_score,
            chunk.score_signals.enriched_score,
            chunk.score_signals.semantic_score.value_or(0.5f));
    }
    (void)query;  // Phase 2: recency / query-aware signal hook
    return base_score;
}

}  // namespace cortrix::reranker
