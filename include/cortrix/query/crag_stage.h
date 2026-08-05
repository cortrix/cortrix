#pragma once
#include "cortrix/query/cross_ns_response.h"
#include "cortrix/query/query_context.h"
#include "cortrix/retrieval/crag_evaluator.h"

namespace cortrix::query {

/// CragStage — the CRAG retrieval-quality evaluation applied AFTER rerank on
/// the live query path (Q6 wiring).
///
/// Placement: CRAG evaluates the FINAL reranked result set, so it
/// runs on the post-ScatterGather CrossNsResponse (cross-NS merged + reranked),
/// gated by ShouldSkipF37 (complex route only; simple/chat skip). The frozen
/// CragEvaluator writes the verdict onto QueryContext but does NOT mutate the result
/// set — that is the Query-Engine's job (§6.3 "downstream"). This stage is that
/// downstream: it converts the ResultItems to RankedChunks, runs
/// EvaluateAndUpdateContext, then applies the verdict:
///   - correct  (≥ threshold_correct): keep the reranker top-K as-is.
///   - ambiguous: keep the top-N/2 by rerank_score (fine-grained filter, §6.3).
///   - incorrect (< threshold_incorrect): Phase 1 keeps the degraded top-K (OBS
///     counter already recorded by the evaluator); no Web fallback in Phase 1.
/// The resolved verdict is exposed via qctx.crag_verdict for the caller to surface
/// as meta.crag_verdict (B-class). On a missing/failed classifier the evaluator
/// degrades to the "correct" path (R4 — heuristic/fallback), so the link is safe.
class CragStage {
public:
    explicit CragStage(retrieval::CragEvaluator* evaluator) : evaluator_(evaluator) {}

    /// Evaluate `resp` for `qctx.query` and apply the §6.3 verdict action in place.
    /// No-op when qctx.enable_crag is false, ShouldSkipF37(qctx) (simple/chat), or
    /// the evaluator is null. Writes qctx.crag_verdict / crag_score /
    /// ambiguous_action_taken. Never throws.
    void Apply(CrossNsResponse& resp, QueryContext& qctx);

private:
    retrieval::CragEvaluator* evaluator_;
};

}  // namespace cortrix::query
