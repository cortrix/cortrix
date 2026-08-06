#pragma once
#include <string>

namespace cortrix::retrieval {

/// CragConfig — resolved configuration for the CRAG evaluator (NS
/// metadata.crag_config + ctor). Phase 1: the effective config is the fixed
/// defaults below, optionally overridden per-namespace by the JSONB crag_config
/// column (the catalog NS-config resolution path is wired later; this
/// struct is the in-memory shape both the defaults and any future resolver fill).
///
/// Threshold semantics (three-tier split):
///   crag_score >= threshold_correct                → "correct"
///   threshold_incorrect <= crag_score < threshold_correct → "ambiguous"
///   crag_score < threshold_incorrect               → "incorrect"
/// Invariant: 0.0 <= threshold_incorrect <= threshold_correct <= 1.0
/// (violations → CX_ERR_CRAG_THRESHOLD_INVALID, validated by IsValid()).
struct CragConfig {
    bool enabled = true;                  ///< L1: NS-config path switch (default on / zero-config)
    float threshold_correct = 0.7f;       ///< NS-configurable upper boundary
    float threshold_incorrect = 0.3f;     ///< NS-configurable lower boundary

    /// Below this classifier confidence, fall back to HeuristicGuard (step 3).
    float classifier_min_confidence = 0.5f;

    /// "small_classifier" / "hybrid" (LLM judge, Cloud V2).
    std::string evaluation_method = "small_classifier";

    bool fallback_to_correct_on_failure = true;  ///< fail-safe default
    bool log_incorrect_to_obs = true;            ///< observability counter

    /// Phase 1 "raw_results_only" (NullRetrievalFallback) / Phase 2 "web_search".
    std::string retrieval_fallback_strategy = "raw_results_only";

    /// L3 transient-failure retry budget (exponential back-off 50/100/200ms).
    int max_inference_retries = 3;

    /// True iff the thresholds satisfy the invariant (in range + ordered).
    /// Callers raise CX_ERR_CRAG_THRESHOLD_INVALID when this is false.
    bool IsValid() const {
        return threshold_incorrect >= 0.0f &&
               threshold_correct <= 1.0f &&
               threshold_incorrect <= threshold_correct;
    }
};

}  // namespace cortrix::retrieval
