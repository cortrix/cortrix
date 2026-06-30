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
        // F07 reserves semantic_score=0.0 as the persisted anomaly sentinel; all
        // non-anomalous write-time levels are in [0.2, 1.0]. Preserve the D6/M2
        // priority rule here so an enriched_score cannot re-boost an anomalous
        // block after retrieval.
        const bool is_anomalous =
            chunk.score_signals.semantic_score.has_value() &&
            *chunk.score_signals.semantic_score == 0.0f;
        return cortrix::scoring::SemanticScorer::ComputeFinalScore(
            base_score,
            chunk.score_signals.enriched_score,
            chunk.score_signals.semantic_score.value_or(0.5f),
            is_anomalous);
    }
    (void)query;  // Phase 2: recency / query-aware signal hook
    return base_score;
}

}  // namespace cortrix::reranker
