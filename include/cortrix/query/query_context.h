#pragma once
#include <map>
#include <string>

namespace cortrix::query {

/// QueryContext — the per-request execution context threaded through the
/// Cross-NS query link (F04 §2.1 / §2.2). It is the immutable bundle a
/// SingleUnitExecutor needs to run one NS's pipeline (Vector+BM25 → RRF →
/// Reranker → top_N) and that ScatterGather hands to every NS's
/// `IScatterExecutor::ExecuteForNamespace`.
///
/// Clarification (Lead ruling): F04 *defines* this type in `cortrix::query` so it lines up
/// with catalog's forward declaration (`catalog/i_unit_scatter_executor.h`
/// forward-declares `cortrix::query::QueryContext` / `UnitQueryResult`). F04 does
/// NOT define `UnitQueryResult` (that is the Unit-level Phase-2 type owned by the
/// catalog MultiUnitNSExecutor); F04's NS-level result is
/// `cortrix::retrieval::NamespaceQueryResult` (RETRIEVAL_TYPES_SPEC §1-bis), which
/// is a different concept.
///
/// V6 D-33 (§2.2): ScatterGather.Execute takes an *optional* `QueryContext*` so a
/// caller (F37 CRAG / F02 Reranker) can share one; when null, ScatterGather builds
/// the context from the QueryRequest + AuthContext. The reranker toggle and filter
/// are passed through to every NS unchanged (§4.1 / topic 4.3).
///
/// Wave C unification (F37/F39 — QUERY_CONTEXT_SPEC.md SoT): the Wave-C joint
/// design (G3+G1.2 §S3) made QueryContext the single query-path context object that
/// also carries the routing + CRAG decision signals exposed via the
/// ARCH `?explain=true` endpoint. QUERY_CONTEXT_SPEC.md §2 is the SoT for those
/// fields. They are added here as a **pure ADD** (C-R1 briefing red-line 2): the
/// original F04 execution fields above are unchanged, so F04's existing consumers
/// (ScatterGather / SingleUnitExecutor / RagFusion) are untouched.
///   - This round F37 writes only its 6 fields (§2.3, see CragEvaluator
///     EvaluateAndUpdateContext). F39's 5 routing fields (§2.2) are declared with
///     defaults; their write logic lands with F39 (Wave C-R2).
///   - All fields have safe defaults (QUERY_CONTEXT_SPEC §6.1: ""/0.0/false/{}) so
///     `?explain=true` never returns null.
struct QueryContext {
    std::string query;       ///< the user/Agent query text (passed through to every NS) — also SPEC §2.1
    int  top_k = 10;         ///< per-NS top_k requested by the caller (range 1-100)
    bool rerank = true;      ///< topic 4.3 — pass-through to all NS; false → RRF fallback path
    std::map<std::string, std::string> filter;  ///< topic 4.3 JSONB filter passed through (flattened)

    // --- Retrieval route switches ---
    // Cross-NS diagnostic/profile controls. Defaults preserve existing behavior:
    // dense vector + FTS5/BM25 + optional F40 sparse all contribute to the chunk-level
    // RRF candidate set. Benchmarks can disable individual routes to produce strict
    // ablations such as dense-only embedding recall.
    bool enable_vector = true;
    bool enable_bm25 = true;
    bool enable_sparse = true;
    bool enable_crag = true;    ///< F37 CRAG post-rerank evaluation/action; diagnostic switch

    // --- F41 §6.2 retrieval granularity ---
    // Passed through ScatterGather to every NS's executor unchanged (like rerank /
    // filter). "chunk" is the explicit chunk-level baseline; "doc" uses doc-summary
    // recall; "both" and the default "auto" run the hybrid fallback so doc-summary
    // candidates can enter the official query candidate set. ScatterGather itself never
    // branches on this — it is a per-NS executor concern (the cross-NS gather/dedupe stays
    // granularity-agnostic).
    std::string granularity = "auto";  ///< "auto" | "chunk" | "doc" | "both"

    // --- Identity pass-through (topic §11.bis MEM05-4: AuthContext.user_id flows through) ---
    std::string user_id;     ///< passed through for downstream isolation / audit (empty in CE single-tenant)
    std::string tenant_id;   ///< passed-through tenant scope (empty/"" = CE single-tenant)

    // --- Trace pass-through (OBS_SPEC §5.3, optional) ---
    std::string trace_id;    ///< empty when no TraceContext was supplied

    // ====================================================================
    // QUERY_CONTEXT_SPEC.md §2 — Wave C query-path decision signals (pure ADD)
    // ====================================================================

    // --- §2.1 base (F39 upstream init; `query` above is the other base field) ---
    std::string ns_id;       ///< target namespace ID (TEXT/ULID, per F34). SPEC §2.1.

    // --- §2.2 F39 writes (routing decision; write logic = Wave C-R2) ---
    std::string routing_path;            ///< "simple" / "complex" / "chat" (SPEC §2.2)
    float complexity_score = 0.0f;       ///< classifier softmax confidence
    std::string routing_decision_source; ///< 8-value enum (SPEC §2.2: rule/llm/force_route/.../chat_demoted_guard)
    bool chat_path_triggered = false;
    bool multi_turn_context_warning = false;  ///< F39-6 (Agent-framework responsibility)

    // --- §2.3 F37 writes (CRAG evaluation; this round) ---
    std::string crag_verdict;            ///< "correct"/"ambiguous"/"incorrect"/"correct_fallback_classifier_failed"
    float crag_score = 0.0f;             ///< softmax confidence
    std::map<std::string, float> f37_signals;  ///< top1/median/std/high_score_ratio/...
    std::string ambiguous_action_taken;  ///< "filtered_top_n_by_2" / "marked_meta_only"
    bool web_fallback_triggered = false; ///< Phase 1 always false (Phase 2 Web fallback → true)
    bool routing_misclassified = false;  ///< Phase 1 always false (Phase 2 bidirectional feedback → true)

    // --- §2.bis presentation flag (pass-through to the per-NS executor) ---
    bool explain = false;  ///< ?explain=true — per-NS executors attach B/C-class
                           ///< explain detail (e.g. chunk-level RRF `rrf_paths`,
                           ///< §3.8 W2) only when set. Default off (no overhead).
};

}  // namespace cortrix::query
