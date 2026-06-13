#include "cortrix/reranker/score_fusion.h"

namespace cortrix::reranker {

float RerankerScoreFusion::ComputeRerankRrfScore(
    float rerank_score, float rrf_score,
    const cortrix::retrieval::RankedChunk& chunk,
    const std::string& query) const {
    // V1 simplified fusion (F02 §4.2-ter): weighted_sum of the cross-encoder
    // precision score and the F36 RRF recall score. Returns a normalized [0,1]
    // ordering score when both inputs are in [0,1].
    (void)chunk;  // Phase 2: business-hotness (F43) signal hook
    (void)query;  // Phase 2: recency / query-aware signal hook
    return rerank_score * kRerankWeight + rrf_score * kRrfWeight;
}

}  // namespace cortrix::reranker
