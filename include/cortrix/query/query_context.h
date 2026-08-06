#pragma once
#include <map>
#include <string>

namespace cortrix::query {

/// QueryContext — the per-request execution context threaded through the
/// Cross-NS query link. It is the immutable bundle a
/// SingleUnitExecutor needs to run one NS's pipeline (Vector+BM25 → RRF →
/// Reranker → top_N) and that ScatterGather hands to every NS's
/// `IScatterExecutor::ExecuteForNamespace`.
///
/// Clarification (Lead ruling): the cross-NS layer *defines* this type in `cortrix::query` so it lines up
/// with catalog's forward declaration (`catalog/i_unit_scatter_executor.h`
/// forward-declares `cortrix::query::QueryContext` / `UnitQueryResult`). It does
/// NOT define `UnitQueryResult` (that is the Unit-level Phase-2 type owned by the
/// catalog MultiUnitNSExecutor); the NS-level result is
/// `cortrix::retrieval::NamespaceQueryResult` (the retrieval-types spec), which
/// is a different concept.
///
/// V6 D-33: ScatterGather.Execute takes an *optional* `QueryContext*` so a
/// caller (CRAG / Reranker) can share one; when null, ScatterGather builds
/// the context from the QueryRequest + AuthContext. The reranker toggle and filter
/// are passed through to every NS unchanged (/ topic 4.3).
///
/// Unification of the CRAG and routing fields: the joint
/// design (G3+G1.2) made QueryContext the single query-path context object that
/// also carries the routing + CRAG decision signals exposed via the
/// ARCH `?explain=true` endpoint. the query-context spec is the SoT for those
/// fields. They are added here as a **pure ADD** (C-R1 briefing red-line 2): the
/// original execution fields above are unchanged, so existing consumers
/// (ScatterGather / SingleUnitExecutor / RagFusion) are untouched.
///   - This round CRAG writes only its 6 fields (see CragEvaluator
///     EvaluateAndUpdateContext). The 5 routing fields are declared with
///     defaults; their write logic lands with the router.
///   - All fields have safe defaults (the query-context spec: ""/0.0/false/{}) so
///     `?explain=true` never returns null.
struct QueryContext {
    std::string query;       ///< the user/Agent query text (passed through to every NS) — also SPEC
    int  top_k = 10;         ///< per-NS top_k requested by the caller (range 1-100)
    bool rerank = true;      ///< topic 4.3 — pass-through to all NS; false → RRF fallback path
    std::map<std::string, std::string> filter;  ///< topic 4.3 JSONB filter passed through (flattened)

    // --- Retrieval route switches ---
    // Cross-NS diagnostic/profile controls. Defaults preserve existing behavior:
    // dense vector + FTS5/BM25 + optional sparse all contribute to the chunk-level
    // RRF candidate set. Benchmarks can disable individual routes to produce strict
    // ablations such as dense-only embedding recall.
    bool enable_vector = true;
    bool enable_bm25 = true;
    bool enable_sparse = true;
    bool enable_crag = true;    ///< CRAG post-rerank evaluation/action; diagnostic switch

    // --- retrieval granularity ---
    // Passed through ScatterGather to every NS's executor unchanged (like rerank /
    // filter). "chunk" is the explicit chunk-level baseline; "doc" uses doc-summary
    // recall; "both" and the default "auto" run the hybrid fallback so doc-summary
    // candidates can enter the official query candidate set. ScatterGather itself never
    // branches on this — it is a per-NS executor concern (the cross-NS gather/dedupe stays
    // granularity-agnostic).
    std::string granularity = "auto";  ///< "auto" | "chunk" | "doc" | "both"

    // --- Identity pass-through (AuthContext.user_id flows through) ---
    std::string user_id;     ///< passed through for downstream isolation / audit (empty in CE single-tenant)
    std::string tenant_id;   ///< passed-through tenant scope (empty/"" = CE single-tenant)

    // --- Trace pass-through (OBS_SPEC, optional) ---
    std::string trace_id;    ///< empty when no TraceContext was supplied

    // ====================================================================
    // the query-context spec — Wave C query-path decision signals (pure ADD)
    // ====================================================================

    // --- Base fields (router-initialized; `query` above is the other base field) ---
    std::string ns_id;       ///< target namespace ID (TEXT/ULID).

    // --- Router writes (routing decision) ---
    std::string routing_path;            ///< "simple" / "complex" / "chat" (SPEC)
    float complexity_score = 0.0f;       ///< classifier softmax confidence
    std::string routing_decision_source; ///< 8-value enum (SPEC: rule/llm/force_route/.../chat_demoted_guard)
    bool chat_path_triggered = false;
    bool multi_turn_context_warning = false;  ///< Agent-framework responsibility

    // --- CRAG writes (evaluation) ---
    std::string crag_verdict;            ///< "correct"/"ambiguous"/"incorrect"/"correct_fallback_classifier_failed"
    float crag_score = 0.0f;             ///< softmax confidence
    std::map<std::string, float> crag_signals;  ///< top1/median/std/high_score_ratio/...
    std::string ambiguous_action_taken;  ///< "filtered_top_n_by_2" / "marked_meta_only"
    bool web_fallback_triggered = false; ///< Phase 1 always false (Phase 2 Web fallback → true)
    bool routing_misclassified = false;  ///< Phase 1 always false (Phase 2 bidirectional feedback → true)

    // --- presentation flag (pass-through to the per-NS executor) ---
    bool explain = false;  ///< ?explain=true — per-NS executors attach B/C-class
                           ///< explain detail (e.g. chunk-level RRF `rrf_paths`,
                           ///< W2) only when set. Default off (no overhead).
};

}  // namespace cortrix::query
