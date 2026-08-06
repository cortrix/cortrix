#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/observability/trace_context.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/query_variant_generator.h"
#include "cortrix/query/rag_fusion_types.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/retrieval/types.h"

namespace cortrix::query {

/// RagFusion — the RAG-Fusion main service. Expands one query into [original + N
/// variants] (LLM), then RRF-fuses the per-variant retrieval results into one
/// globally-ranked list (topic 2 B: a single global second-pass fusion, NOT per-NS).
///
/// 🔑 RRF reuse note (B_R1_BRIEFING §7): the frozen `cortrix::RRFFusion`
/// (query/rrf_fusion.h) fuses `RouteResult` → `ScoredBlock` keyed by int64
/// block_id — that is the INNER per-NS fusion of vector/BM25 routes that the cross-NS layer
/// already performs. FuseResults is the OUTER second-pass fusion across
/// variants, over already-fused `cortrix::retrieval::ScoredResult` (keyed by the
/// string ChildId). This applies an anchored
/// conservative weighted RRF formula over that keyspace: `per_variant_results[0]`
/// is the original query, receives the primary role weight, and its top-3 head is
/// emitted first as a strong-hit guardrail; LLM variants receive weaker additive
/// weights for the remaining order so variants can improve recall without
/// displacing the original query's strongest evidence. The injected RRFFusion
/// (held per the §4.3 ctor + §10.1 DI) carries the resolved default k; FuseResults
/// takes an explicit rrf_k so the NS-config value (§4.4) wins.
class RagFusion {
public:
    /// @param generator the LLM variant generator (§4.2)
    /// @param rrf       the shared RRFFusion (§4.3 ctor / §10.1 DI; carries the
    ///                  global default k). Must be non-null.
    RagFusion(std::shared_ptr<QueryVariantGenerator> generator,
              std::shared_ptr<RRFFusion> rrf);

    /// Expand `query` → [original query] + N variants (§4.3). topic 4: on LLM
    /// failure returns the CX_ERR_RAG_FUSION_* Status (the caller degrades to the
    /// single query + emits CX_WARN_RAG_FUSION_DEGRADED). When the NS config is
    /// disabled, or routing says skip, returns just `{query}` (Ok, no LLM call).
    ///
    /// v1.0.5 canonical 4-param signature (V2 QA M-05): trace_ctx + qctx both
    /// optional. The result always begins with `query` itself (§4.5 invariant).
    ///
    /// 🔵 integration deferred: the `qctx` routing-skip (ShouldSkipF36 reading
    /// SPEC `routing_path`) is INTERFACE-RESERVED — the FROZEN QueryContext has no
    /// `routing_path` field yet (the router adds it, not frozen). So a non-null
    /// `qctx` is accepted and threaded, but the real skip wiring lands at D3.5 when
    /// that field exists; until then `qctx` does not change behavior.
    Result<std::vector<std::string>> ExpandQueries(
        const std::string& query,
        const RagFusionConfig& ns_config,
        const observability::TraceContext* trace_ctx = nullptr,
        const query::QueryContext* qctx = nullptr);

    /// Global RRF second-pass fusion of the per-variant retrieval results (§4.3 /
    /// topic 2 B). `per_variant_results[i]` is variant i's ranked candidates (each
    /// list assumed sorted best-first; rank = position). Contract: index 0 is the
    /// original query; indexes 1..N are LLM variants. Returns one fused,
    /// de-duplicated (by ChildId, keep-sum) list with the original query's top-3
    /// head anchored first, followed by remaining candidates sorted by weighted RRF
    /// score descending.
    ///
    /// @param rrf_k RRF constant (§4.4 NS value; default 60). k ≤ 0 → treated as 60.
    Result<std::vector<retrieval::ScoredResult>> FuseResults(
        const std::vector<std::vector<retrieval::ScoredResult>>& per_variant_results,
        int rrf_k = 60,
        const observability::TraceContext* ctx = nullptr);

    /// v1.0.13 (§4.3.bis.5) config-aware overload: reads rrf_k plus the fusion
    /// policy knobs (fusion_original_weight / fusion_variant_weight /
    /// fusion_anchor_max) from `cfg`. The rrf_k-only overload above delegates
    /// here with a default-constructed config (byte-identical v1.0.7 policy).
    Result<std::vector<retrieval::ScoredResult>> FuseResults(
        const std::vector<std::vector<retrieval::ScoredResult>>& per_variant_results,
        const RagFusionConfig& cfg,
        const observability::TraceContext* ctx);

    /// RAG-Fusion path state for the explain endpoint.
    /// Only surfaced via ?explain=true — never in a default query response.
    struct ExplainState {
        bool active = false;
        std::string reason;             ///< "no_llm_configured" / "ns_disabled" /
                                        ///< "skipped_by_F39_routing" / "active"
        int variant_count = 0;          ///< actual variant count executed (degraded → 1)
        std::vector<std::string> variants_used;  ///< returned only on explain
        bool degraded = false;
        std::string degrade_reason;     ///< "llm_timeout" / "llm_http" / ...
        std::string degrade_detail;     ///< safe hint: e.g. "http_status=401"
        std::optional<int> llm_latency_ms;
    };

    /// The current explain state (reflects the most recent ExpandQueries call on
    /// this instance). Pure accessor — no side effects.
    ExplainState GetExplainState() const { return explain_; }

    /// Mark the current request as skipped before LLM expansion by an activation
    /// policy (v1.0.11 selective activation). This keeps ?explain=true from
    /// exposing stale state from a previous ExpandQueries call.
    void MarkSkippedByActivationPolicy(const std::string& query,
                                       const std::string& reason);

private:
    std::shared_ptr<QueryVariantGenerator> generator_;
    std::shared_ptr<RRFFusion> rrf_;
    ExplainState explain_;  ///< updated by ExpandQueries (§4.3)
};

}  // namespace cortrix::query
