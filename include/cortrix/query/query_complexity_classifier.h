#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/query/complexity_classifier_backend.h"  // IComplexityClassifierBackend
#include "cortrix/query/complexity_config.h"              // ComplexityConfig
#include "cortrix/retrieval/classifier.h"                 // IClassifier / ClassifierInput / ClassificationResult

namespace cortrix::query {

struct QueryContext;  // fwd-decl (defined in query/query_context.h); RouteAndUpdateContext only needs the reference

/// QueryComplexityClassifier — the Adaptive-RAG query-complexity router
/// It implements the shared IClassifier contract (ruling of the
/// G3+G1.2 joint D1 — declared in cortrix::retrieval, reused here) and the
/// F39-specific RouteAndUpdateContext that writes the routing decision onto
/// QueryContext (S3 ruling).
///
/// It lives in cortrix::query (not cortrix::retrieval) because it is a *query-path*
/// component: the standalone routing mock left by the CRAG stage
/// (retrieval/routing_mock.h) names the real owner as
/// `cortrix::query::QueryComplexityClassifier::ShouldSkipF37`, and the QueryPipeline
/// drives it before RAG-Fusion, rerank and CRAG. Implementing retrieval::IClassifier across the
/// namespace boundary is intentional and matches that documented contract.
///
/// Routing algorithm (§6.1, in order):
///   1. Agent ?route override (force_route arg)        → routing_decision_source="force_route"
///   2. NS-config force_route override                 → "ns_force_route"
///   3. heuristic chat rule guard (IsChatQuery)        → "rule" (Chat)
///   4. main path: classifier backend Infer            → "llm" (DistilBERT-tiny, ML)
///   5. confidence-threshold fail-safe (< threshold)   → "low_confidence_fallback" (Complex)
///   6. write routing_path / complexity_score / chat_path_triggered
///   7. multi-turn signal detection                    → multi_turn_context_warning
///   L3 transient inference fault → retry then default to Complex ("inference_failed_fallback").
///   L1 (NS disabled) / L2 (backend unavailable) → default to Complex ("classifier_unavailable").
///
/// Standalone (D3): the backend is a HeuristicComplexityBackend stub (no ONNX); the
/// real DistilBERT-tiny OnnxComplexityBackend + QueryPipeline step-3 wiring are
/// The skip decisions are exposed via the static ShouldSkipF36/ShouldSkipF37
/// helpers the QueryPipeline calls (this class never reaches into those stages).
class QueryComplexityClassifier : public retrieval::IClassifier {
public:
    /// @param backend  inference backend (HeuristicComplexityBackend standalone /
    ///                 OnnxComplexityBackend in D3.5). May be null → IsAvailable()
    ///                 ==false and routing takes the L2 "classifier_unavailable"
    ///                 default-Complex path (after the rule/force guards).
    /// @param config   resolved ComplexityConfig (defaults or NS-override).
    QueryComplexityClassifier(std::shared_ptr<IComplexityClassifierBackend> backend,
                              const ComplexityConfig& config);

    // --- IClassifier (S1 shared contract) ---
    /// Classify the complexity of `input.query`. Total (never throws): a transient
    /// backend fault is caught and reported via the safe Complex fallback label +
    /// ClassificationResult.error_code = CX_ERR_ROUTER_INFERENCE_FAILED. `input.chunks`
    /// is ignored — this is a *pre-retrieval* router, but it honors the shared
    /// post-retrieval IClassifier signature so the pipeline treats all classifiers
    /// uniformly.
    retrieval::ClassificationResult Classify(const retrieval::ClassifierInput& input) override;
    std::string Name() const override { return "f39-complexity"; }
    std::vector<std::string> Labels() const override {
        return {"simple", "complex", "chat"};
    }
    bool IsAvailable() const override;

    /// F39-specific (§6.1): route `ctx.query` and write the routing fields back onto
    /// `ctx` (routing_path / complexity_score / routing_decision_source /
    /// chat_path_triggered / multi_turn_context_warning), honoring an optional
    /// Agent `?route=` override and the resolved NS ComplexityConfig.
    ///
    /// Returns Status::Ok on every successful route (including all fail-safe
    /// degradations, which are normal). Returns InvalidArgument carrying
    /// CX_ERR_ROUTER_FORCE_ROUTE_INVALID when `force_route` (or the NS force_route) is
    /// not one of auto/simple/complex/chat — ctx is left untouched in that case so
    /// the caller can surface the Agent-friendly error.
    Status RouteAndUpdateContext(
        QueryContext& ctx,
        const std::optional<std::string>& force_route = std::nullopt);

    // --- Skip hooks: the QueryPipeline calls these *before* RAG-Fusion / CRAG to
    // decide the skip. Static + pure (depend only on the already-written ctx) so
    // they are trivially testable and have no instance state. ---

    /// RAG-Fusion is skipped on the Simple and Chat paths: both
    /// keep the single original query (no LLM variant expansion). An empty
    /// routing_path (not yet routed) → do NOT skip (fail-safe to running it).
    static bool ShouldSkipF36(const QueryContext& ctx);

    /// CRAG runs only on the Complex / (default) path; it is skipped on Simple
    /// and Chat. Same fail-safe as ShouldSkipF36. Kept in
    /// exact lockstep with the F37-side mock (retrieval/routing_mock.h).
    static bool ShouldSkipF37(const QueryContext& ctx);

    // --- Exposed for unit tests / reuse ---

    /// Heuristic chat rule guard (§6.1 step 3): true for trivial greetings /
    /// acknowledgments with no retrievable content ("hi", "thanks", CJK greetings, …).
    /// Conservative by design (§14 risk): only an exact-ish match of a short
    /// pleasantry routes to Chat, because Chat skips all retrieval.
    static bool IsChatQuery(const std::string& query);

    /// Multi-turn / anaphora signal detection (§6.2, borrowed 6): true when the
    /// query leans on prior-turn context (pronouns / demonstratives / follow-up
    /// cues). Cortrix does NOT rewrite the query — it only sets
    /// multi_turn_context_warning so the Agent framework can react.
    static bool HasMultiTurnSignal(const std::string& query);

private:
    /// Run the backend with the §7.3 L3 retry-then-degrade policy. On success
    /// returns the backend result with error_code unset; after `max_inference_retries`
    /// transient faults returns the degraded Complex result (error_code =
    /// CX_ERR_ROUTER_INFERENCE_FAILED). Never throws.
    retrieval::ClassificationResult RunClassifier(const std::string& query);

    /// The §7.3 transparent-degrade result: Complex label, confidence 0.5,
    /// error_code CX_ERR_ROUTER_INFERENCE_FAILED.
    static retrieval::ClassificationResult DegradedResult();

    std::shared_ptr<IComplexityClassifierBackend> backend_;
    ComplexityConfig config_;
};

}  // namespace cortrix::query
