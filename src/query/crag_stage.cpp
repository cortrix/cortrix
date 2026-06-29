#include "cortrix/query/crag_stage.h"

#include <vector>

#include "cortrix/query/query_complexity_classifier.h"
#include "cortrix/retrieval/types.h"

namespace cortrix::query {

void CragStage::Apply(CrossNsResponse& resp, QueryContext& qctx) {
    // Skip when F39 says so (simple / chat routes) or no evaluator is wired. F37
    // runs only on the Complex path (§6.3 / F39 ShouldSkipF37) — keep this gate in
    // exact lockstep with the F39 helper so the route decision drives it.
    if (evaluator_ == nullptr) return;
    if (QueryComplexityClassifier::ShouldSkipF37(qctx)) return;

    // Convert the post-rerank ResultItems to the RankedChunks the evaluator consumes
    // (RETRIEVAL_TYPES_SPEC §1: child_id + chunk_text + scores). The multi-signal
    // feature engineering reads rerank_score + chunk_text.
    std::vector<retrieval::RankedChunk> chunks;
    chunks.reserve(resp.results.size());
    for (const auto& it : resp.results) {
        retrieval::RankedChunk rc;
        rc.child_id = it.child_id;
        rc.chunk_text = it.content;
        rc.parent_text = it.parent_content;
        rc.score = it.score;
        rc.rerank_score = it.rerank_score;
        rc.score_signals = it.score_signals;
        chunks.push_back(std::move(rc));
    }

    // §6.3: classify + write qctx.crag_verdict / crag_score / ambiguous_action_taken.
    // The evaluator records the OBS metric and takes the verdict's internal path;
    // it does NOT mutate the result set — that is applied below.
    evaluator_->EvaluateAndUpdateContext(qctx, chunks);

    // §6.3 verdict actions on the result set:
    //   - ambiguous → fine-grained filter: keep the top-N/2 by rerank_score (the
    //     results are already sorted best-first by the cross-NS gather, so this is a
    //     prefix truncation to ceil(N/2)).
    //   - correct / incorrect → keep the reranker top-K as-is (incorrect is a
    //     Phase-1 degraded return; the OBS counter already fired in the evaluator).
    if (qctx.crag_verdict == "ambiguous" && !resp.results.empty()) {
        const std::size_t keep = (resp.results.size() + 1) / 2;  // ceil(N/2)
        if (resp.results.size() > keep) {
            resp.results.resize(keep);
        }
    }
}

}  // namespace cortrix::query
