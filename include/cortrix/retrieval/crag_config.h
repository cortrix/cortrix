#pragma once
#include <string>

namespace cortrix::retrieval {

/// CragConfig — resolved configuration for the F37 CRAG evaluator (F37 §4.2 NS
/// metadata.crag_config + §5.2 ctor). Phase 1: the effective config is the fixed
/// defaults below, optionally overridden per-namespace by the JSONB crag_config
/// column (the F12 NS-config resolution path is cross-Feature wiring → D3.5; this
/// struct is the in-memory shape both the defaults and any future resolver fill).
///
/// Threshold semantics (§6.2 / §1.1 three-tier split):
///   crag_score >= threshold_correct                → "correct"
///   threshold_incorrect <= crag_score < threshold_correct → "ambiguous"
///   crag_score < threshold_incorrect               → "incorrect"
/// Invariant: 0.0 <= threshold_incorrect <= threshold_correct <= 1.0
/// (violations → CX_ERR_F37_THRESHOLD_INVALID, validated by IsValid()).
struct CragConfig {
    bool enabled = true;                  ///< §7.1 L1: NS-config path switch (default on / zero-config)
    float threshold_correct = 0.7f;       ///< §4.2 F37-2 NS-configurable upper boundary
    float threshold_incorrect = 0.3f;     ///< §4.2 F37-2 NS-configurable lower boundary

    /// Below this classifier confidence, fall back to HeuristicGuard (§6.2 step 3).
    float classifier_min_confidence = 0.5f;

    /// §4.2 "small_classifier" (F37-1 B) / "hybrid" (F37-1 E LLM judge, Cloud V2).
    std::string evaluation_method = "small_classifier";

    bool fallback_to_correct_on_failure = true;  ///< §4.2 / §7.3 F37-8 default
    bool log_incorrect_to_obs = true;            ///< §4.2 F37-5 OBS counter

    /// §4.2 Phase 1 "raw_results_only" (NullRetrievalFallback) / Phase 2 "web_search".
    std::string retrieval_fallback_strategy = "raw_results_only";

    /// L3 transient-failure retry budget (§7.3 — exponential back-off 50/100/200ms).
    int max_inference_retries = 3;

    /// True iff the thresholds satisfy the §6.2 invariant (in range + ordered).
    /// Callers raise CX_ERR_F37_THRESHOLD_INVALID when this is false.
    bool IsValid() const {
        return threshold_incorrect >= 0.0f &&
               threshold_correct <= 1.0f &&
               threshold_incorrect <= threshold_correct;
    }
};

}  // namespace cortrix::retrieval
