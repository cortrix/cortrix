#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/retrieval/classifier.h"               // IClassifier / ClassifierInput / ClassificationResult
#include "cortrix/retrieval/crag_classifier_backend.h"  // ICragClassifierBackend
#include "cortrix/retrieval/crag_config.h"              // CragConfig
#include "cortrix/retrieval/types.h"                    // RankedChunk
#include "cortrix/reranker/circuit_breaker.h"           // CircuitBreaker (reusable)

namespace cortrix::query {
struct QueryContext;  // fwd-decl (defined in query/query_context.h); EvaluateAndUpdateContext only needs the reference
}  // namespace cortrix::query

namespace cortrix::retrieval {

/// CragEvaluator — the CRAG retrieval-quality evaluator.
/// Implements the shared IClassifier contract and the CRAG-specific
/// EvaluateAndUpdateContext that writes the verdict back onto QueryContext.
///
/// Pipeline (§6.2): ComputeMultiSignals → heuristic guard (degenerate input) →
/// backend Infer → low-confidence guard → three-tier threshold mapping. Backend
/// faults go through an L3 retry/degrade path (§7.3): retry max_inference_retries
/// times, then transparently return the all-Correct fallback verdict
/// (label "correct", error_code CX_ERR_CRAG_INFERENCE_FAILED).
///
/// Standalone (D3): the backend is a HeuristicGuardBackend stub (no ONNX); the
/// real DistilBERT-tiny OnnxCragBackend + QueryPipeline step-10 wiring are D3.5.
class CragEvaluator : public IClassifier {
public:
    /// @param backend  inference backend (HeuristicGuardBackend standalone /
    ///                 OnnxCragBackend in D3.5). May be null → IsAvailable()==false
    ///                 and Classify() takes the heuristic-only path.
    /// @param config   resolved CragConfig (defaults or NS-override).
    CragEvaluator(std::shared_ptr<ICragClassifierBackend> backend,
                  const CragConfig& config);

    // --- IClassifier ---
    ClassificationResult Classify(const ClassifierInput& input) override;
    std::string Name() const override { return "crag"; }
    std::vector<std::string> Labels() const override {
        return {"correct", "ambiguous", "incorrect"};
    }
    bool IsAvailable() const override;

    /// CRAG-specific (§6.3): classify `chunks` for `ctx.query` and write the verdict
    /// fields back onto `ctx` (crag_verdict / crag_score / crag_signals /
    /// ambiguous_action_taken / web_fallback_triggered). Path handling per §6.3
    /// (correct: no-op return; ambiguous: mark action; incorrect: OBS counter +
    /// degrade). Does NOT decide skip — that is the QueryPipeline's job via
    /// ShouldSkipCrag. The real definition lands with the router.
    void EvaluateAndUpdateContext(query::QueryContext& ctx,
                                  const std::vector<RankedChunk>& chunks);

    // --- Exposed for unit tests / reuse (multi-signal feature engineering, §6.1) ---
    std::map<std::string, float> ComputeMultiSignals(
        const std::vector<RankedChunk>& chunks) const;

private:
    /// Map a verdict score through the configured thresholds to a label (§6.2).
    std::string LabelFromScore(float score) const;

    /// Heuristic guard (§6.2 step 2 / §7.3): a no-ONNX verdict derived purely from
    /// the multi-signals. Used for degenerate input (empty chunks / tiny query),
    /// low backend confidence, or when no backend is available.
    ClassificationResult HeuristicGuard(
        const std::map<std::string, float>& signals) const;

    /// Run the backend with the L3 retry-then-degrade policy (§7.3).
    ClassificationResult RunClassifier(
        const std::string& query,
        const std::string& chunk_text,
        const std::map<std::string, float>& signals);

    /// The §7.3 transparent-degrade verdict ("correct_fallback_classifier_failed"
    /// + CX_ERR_CRAG_INFERENCE_FAILED), preserving signals for ?explain=true.
    ClassificationResult DegradedResult(
        const std::map<std::string, float>& signals) const;

    std::shared_ptr<ICragClassifierBackend> backend_;
    CragConfig config_;
    reranker::CircuitBreaker breaker_;
};

}  // namespace cortrix::retrieval
