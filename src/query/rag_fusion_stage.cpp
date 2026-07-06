#include "cortrix/query/rag_fusion_stage.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "cortrix/query/rag_fusion_error.h"
#include "cortrix/reranker/score_fusion.h"
#include "cortrix/retrieval/types.h"

namespace cortrix::query {

namespace {

using retrieval::ResultItem;
using retrieval::ScoredResult;

// CrossNsResponse.results → F36 ScoredResult[] (child_id + score). F36 fuses on the
// child_id keyspace (RETRIEVAL_TYPES_SPEC §1); we use the per-item final score as
// the rank carrier (the list is already sorted best-first by the scatter).
std::vector<ScoredResult> ToScoredResults(const CrossNsResponse& resp) {
    std::vector<ScoredResult> out;
    out.reserve(resp.results.size());
    for (const auto& it : resp.results) {
        ScoredResult sr;
        sr.child_id = it.child_id;
        // Carry the post-F02/F07 final score into the second-pass RRF. Older
        // responses may only have rerank_score populated, so keep it as fallback.
        sr.score = it.score != 0.0f ? it.score : it.rerank_score;
        out.push_back(std::move(sr));
    }
    return out;
}

// Build the CX_WARN_RAG_FUSION_DEGRADED warning object (§7 / topic 4) for
// meta.warnings. retry_after_ms comes from the canonical error table.
nlohmann::json DegradedWarning() {
    const auto& info = GetRagFusionErrorInfo(RagFusionErrorCode::kDegraded);
    nlohmann::json w;
    w["code"] = info.cx_code;
    w["reason"] = "rag_fusion_degraded";
    w["category"] = "transient";
    if (info.retry_after_ms.has_value()) w["retry_after_ms"] = *info.retry_after_ms;
    return w;
}

int RequestedTopK(int top_k) {
    return top_k < 1 ? 1 : top_k;
}

int ExpandedCandidateTopK(int top_k, const RagFusionConfig& cfg) {
    const int requested = RequestedTopK(top_k);
    const int multiplier = std::max(kRagFusionCandidateMultiplierMin,
                                    cfg.candidate_multiplier);
    if (multiplier <= 1) return requested;

    long long expanded = static_cast<long long>(requested) * multiplier;
    const int cap = std::max(kRagFusionMaxCandidatesMin, cfg.max_candidates);
    if (expanded > cap) expanded = cap;
    if (expanded < requested) expanded = requested;
    return static_cast<int>(expanded);
}

retrieval::RankedChunk ToRankedChunkForFinalRerank(const ResultItem& item) {
    retrieval::RankedChunk rc;
    rc.child_id = item.child_id;
    rc.chunk_text = item.content;
    rc.parent_text = item.parent_content;
    rc.score = item.score;
    rc.rerank_score = item.rerank_score;
    rc.score_signals = item.score_signals;
    rc.metadata = item.metadata;
    return rc;
}

float EffectiveScore(const ResultItem& item) {
    // `score` is the downstream ordering contract: F02 writes rerank+RRF fused
    // scores back to RankedChunk::score, and ScatterGather sorts ResultItem by
    // score. Use rerank_score only as a compatibility fallback for legacy/fake
    // responses that did not populate the final score.
    return item.score != 0.0f ? item.score : item.rerank_score;
}

void TrimToRequestedTopK(CrossNsResponse* resp, int top_k) {
    if (resp == nullptr) return;
    const int k = RequestedTopK(top_k);
    if (static_cast<int>(resp->results.size()) > k) {
        resp->results.resize(static_cast<std::size_t>(k));
    }
}

bool ShouldSkipLlmBySelectiveMargin(const CrossNsResponse& original,
                                    const RagFusionConfig& cfg) {
    if (cfg.activation_policy != "selective_margin") return false;
    if (original.results.size() <
        static_cast<std::size_t>(cfg.activation_min_results)) {
        return false;
    }
    if (original.results.size() < 2u) return false;

    const float top = EffectiveScore(original.results[0]);
    const float second = EffectiveScore(original.results[1]);
    if (top <= second) return false;
    return (top - second) >= cfg.activation_score_margin;
}

void ApplyFinalRerankIfEnabled(std::vector<ResultItem>* items,
                               const std::string& original_query,
                               const QueryContext& qctx,
                               const RagFusionConfig& cfg,
                               reranker::IReranker* reranker) {
    if (items == nullptr || items->empty()) return;
    if (!cfg.final_rerank || !qctx.rerank || reranker == nullptr) return;

    std::vector<const char*> passages;
    passages.reserve(items->size());
    for (const auto& item : *items) passages.push_back(item.content.c_str());

    std::vector<float> scores = reranker->ScoreBatch(original_query.c_str(), passages);
    const reranker::RerankerScoreFusion score_fusion;
    for (std::size_t i = 0; i < items->size(); ++i) {
        ResultItem& item = (*items)[i];
        if (i < scores.size()) item.rerank_score = scores[i];
        retrieval::RankedChunk rc = ToRankedChunkForFinalRerank(item);
        item.score = score_fusion.ComputeRerankRrfScore(
            item.rerank_score, item.score, rc, original_query);
    }

    std::stable_sort(items->begin(), items->end(),
                     [](const ResultItem& a, const ResultItem& b) {
                         return a.score > b.score;
                     });
}

}  // namespace

CrossNsResponse RagFusionStage::Run(const QueryRequest& request,
                                    const AuthContext& auth,
                                    const QueryContext& qctx,
                                    const RagFusionConfig& cfg) {
    const int candidate_top_k = ExpandedCandidateTopK(request.top_k, cfg);
    CrossNsResponse original_preflight;
    bool has_original_preflight = false;
    if (cfg.activation_policy == "selective_margin") {
        QueryRequest vr = request;
        vr.top_k = candidate_top_k;
        QueryContext vctx = qctx;
        vctx.top_k = candidate_top_k;
        original_preflight = scatter_->Execute(vr, auth, &vctx);
        has_original_preflight = true;
        if (ShouldSkipLlmBySelectiveMargin(original_preflight, cfg)) {
            fusion_->MarkSkippedByActivationPolicy(
                request.query, "skipped_by_activation_policy");
            TrimToRequestedTopK(&original_preflight, request.top_k);
            return original_preflight;
        }
    }

    // 1. Expand the query into [original + N variants] (§4.3). On LLM failure
    //    ExpandQueries returns non-ok → degrade to the single original query +
    //    the CX_WARN_RAG_FUSION_DEGRADED warning (topic 4).
    std::vector<std::string> all_queries = {request.query};
    bool degraded = false;
    auto expanded = fusion_->ExpandQueries(request.query, cfg, /*trace_ctx=*/nullptr, &qctx);
    if (expanded.ok()) {
        // ExpandQueries already returns [original + variants]. Do not prepend the
        // original again; duplicate original queries skew global RRF attribution.
        all_queries = std::move(expanded.value());
    } else {
        degraded = true;
    }

    // Single query (no variants, or expansion degraded) → plain scatter, plus the
    // degraded warning when expansion failed.
    if (all_queries.size() == 1) {
        CrossNsResponse resp;
        if (has_original_preflight) {
            resp = original_preflight;
            TrimToRequestedTopK(&resp, request.top_k);
        } else {
            resp = scatter_->Execute(request, auth, &qctx);
        }
        if (degraded) resp.meta.warnings.push_back(DegradedWarning());
        return resp;
    }

    // 2. Run ScatterGather per query (§4.3 steps 5-8). Keep the FIRST (original)
    //    query's full response as the base — its ResultItems carry the content /
    //    metadata we re-order below (no content re-fetch). Collect each variant's
    //    ScoredResult[] for the global RRF.
    std::vector<std::vector<ScoredResult>> per_variant;
    per_variant.reserve(all_queries.size());
    CrossNsResponse base;
    std::unordered_map<std::string, ResultItem> items_by_child;  // union across variants
    // In final-rerank mode, keep the original query's normal reranked baseline
    // signal, but treat LLM variants as candidate generation only. Running the
    // reranker inside every LLM variant would multiply latency and prematurely
    // truncate variant candidates before the global F36 union. The final pass then
    // reranks the union against the original query.
    const bool defer_inner_rerank = cfg.final_rerank && qctx.rerank && reranker_ != nullptr;
    for (std::size_t i = 0; i < all_queries.size(); ++i) {
        QueryRequest vr = request;
        vr.query = all_queries[i];
        vr.top_k = candidate_top_k;
        if (defer_inner_rerank && i > 0) vr.rerank = false;
        QueryContext vctx = qctx;
        vctx.query = all_queries[i];
        vctx.top_k = candidate_top_k;
        if (defer_inner_rerank && i > 0) vctx.rerank = false;
        CrossNsResponse vresp;
        if (has_original_preflight && i == 0) {
            vresp = original_preflight;
        } else {
            vresp = scatter_->Execute(vr, auth, &vctx);
        }
        per_variant.push_back(ToScoredResults(vresp));
        if (i == 0) base = vresp;  // base meta + the original query's items
        for (auto& it : vresp.results) {
            items_by_child.emplace(it.child_id, it);  // first occurrence wins (rich item)
        }
    }

    // 3. Global RRF second-pass fusion across variants (§4.3 step 9 / topic 2 B).
    auto fused = fusion_->FuseResults(per_variant, cfg.rrf_k, /*ctx=*/nullptr);

    // 4. Re-order the union of ResultItems by the fused order. A fused child that
    //    has no materialized ResultItem (shouldn't happen — every fused child came
    //    from some variant) is skipped. Falls back to the base response on a fusion
    //    fault (defensive — FuseResults is total in practice).
    if (fused.ok()) {
        std::vector<ResultItem> reordered;
        reordered.reserve(fused.value().size());
        for (const auto& sr : fused.value()) {
            auto it = items_by_child.find(sr.child_id);
            if (it == items_by_child.end()) continue;
            ResultItem ri = it->second;
            ri.score = sr.score;  // carry the global RRF score as the final score
            reordered.push_back(std::move(ri));
        }
        ApplyFinalRerankIfEnabled(&reordered, request.query, qctx, cfg, reranker_);
        const int k = RequestedTopK(request.top_k);
        if (static_cast<int>(reordered.size()) > k) {
            reordered.resize(static_cast<std::size_t>(k));
        }
        base.results = std::move(reordered);
    }

    if (degraded) base.meta.warnings.push_back(DegradedWarning());
    return base;
}

}  // namespace cortrix::query
