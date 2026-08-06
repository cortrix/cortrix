#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_context.h"
#include "cortrix/query/cross_ns_request.h"
#include "cortrix/query/cross_ns_response.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/rag_fusion.h"
#include "cortrix/query/rag_fusion_types.h"
#include "cortrix/query/scatter_gather.h"
#include "cortrix/reranker.h"

namespace cortrix::query {

/// RagFusionStage — multi-query expansion + global RRF orchestration over
/// the ScatterGather.
///
/// Activation: runs ONLY when the route is "complex" AND
/// the resolved RagFusionConfig.enabled is true (topic 3 — V1.0 OSS default false).
/// On every other path the caller skips this stage and runs the plain scatter.
///
/// Flow (steps 4-9): ExpandQueries(query) → [original + N variants] (LLM via
/// the injected RagFusion); run ScatterGather.Execute per query; convert each
/// CrossNsResponse to the ScoredResult[] (child_id + score); global RRF
/// FuseResults across variants; then re-rank the union of the per-variant
/// ResultItems by the fused order (keeping the rich ResultItem content/metadata —
/// no content re-fetch). On LLM failure ExpandQueries degrades to the single
/// original query and the response gains meta.warnings[CX_WARN_RAG_FUSION_DEGRADED].
class RagFusionStage {
public:
    /// @param scatter   the ScatterGather (NOT owned).
    /// @param fusion    the RagFusion service (NOT owned).
    /// @param reranker  optional reranker used only for the final rerank
    ///                  after outer fusion; null keeps the pre-v1.0.9 behavior.
    RagFusionStage(ScatterGather* scatter, RagFusion* fusion,
                   reranker::IReranker* reranker = nullptr)
        : scatter_(scatter), fusion_(fusion), reranker_(reranker) {}

    /// Run the expand + per-variant scatter + global RRF fusion. `request` is the
    /// already-parsed request; `qctx` carries the resolved route + execution
    /// fields. Throws nothing the plain scatter wouldn't (CrossNsException for hard
    /// auth/quota failures, surfaced by the caller). Returns the fused response.
    CrossNsResponse Run(const QueryRequest& request, const AuthContext& auth,
                        const QueryContext& qctx, const RagFusionConfig& cfg);

private:
    ScatterGather* scatter_;
    RagFusion* fusion_;
    reranker::IReranker* reranker_;
};

}  // namespace cortrix::query
