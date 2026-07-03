#include "cortrix/query/rag_fusion_stage.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "cortrix/query/rag_fusion_error.h"
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

}  // namespace

CrossNsResponse RagFusionStage::Run(const QueryRequest& request,
                                    const AuthContext& auth,
                                    const QueryContext& qctx,
                                    const RagFusionConfig& cfg) {
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
        CrossNsResponse resp = scatter_->Execute(request, auth, &qctx);
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
    for (std::size_t i = 0; i < all_queries.size(); ++i) {
        QueryRequest vr = request;
        vr.query = all_queries[i];
        QueryContext vctx = qctx;
        vctx.query = all_queries[i];
        CrossNsResponse vresp = scatter_->Execute(vr, auth, &vctx);
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
        const int k = request.top_k < 1 ? 1 : request.top_k;
        if (static_cast<int>(reordered.size()) > k) {
            reordered.resize(static_cast<std::size_t>(k));
        }
        base.results = std::move(reordered);
    }

    if (degraded) base.meta.warnings.push_back(DegradedWarning());
    return base;
}

}  // namespace cortrix::query
