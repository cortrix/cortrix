#pragma once
#include <string>

namespace cortrix::query {

/// ComplexityConfig — resolved configuration for the F39 query-complexity router
/// (F39 §4.2 NS metadata.complexity_config + §5.1 ctor). Phase 1: the effective
/// config is the fixed defaults below, optionally overridden per-namespace by the
/// JSONB `complexity_config` column (the F12 NS-config resolution path is
/// cross-Feature wiring → D3.5; this struct is the in-memory shape both the
/// defaults and any future resolver fill).
///
/// Field semantics map 1:1 to the §4.2 schema:
///   enabled                         — L1 NS-config path switch (default on / zero-config)
///   force_route                     — "auto" / "simple" / "complex" / "chat" (NS-level override)
///   confidence_threshold            — §6.1 step 5: classifier confidence below this → Complex (fail-safe)
///   evaluation_method               — "small_classifier" (F39-1 B) / "hybrid" (Phase 2 LLM judge)
///   fallback_to_complex_on_failure  — §7 L1/L2/L3 default behavior
///   multi_turn_warning_enabled      — §6.2 multi_turn_context_warning detection toggle
///   max_inference_retries           — §7.3 L3 retry budget (exponential back-off 50/100/200ms)
struct ComplexityConfig {
    bool enabled = true;                       ///< §7.1 L1 NS-config path switch
    std::string force_route = "auto";          ///< §4.2 "auto"/"simple"/"complex"/"chat"
    float confidence_threshold = 0.5f;         ///< §4.2 F39-8 NS-configurable [0.3, 0.8]
    std::string evaluation_method = "small_classifier";  ///< §4.2 F39-1 B
    bool fallback_to_complex_on_failure = true;          ///< §4.2 / §7 F39-7 default
    bool multi_turn_warning_enabled = true;    ///< §4.2 F39-6 multi_turn_context_warning toggle
    int max_inference_retries = 3;             ///< §7.3 L3 retry budget

    /// §4.2 F39-8 boundary hard-limit: a NS-supplied confidence_threshold is valid
    /// only inside [0.3, 0.8]. Callers raise CX_ERR_F39_FORCE_ROUTE_INVALID's sibling
    /// validation at config-resolution time (D3.5); the router clamps defensively.
    bool IsThresholdValid() const {
        return confidence_threshold >= 0.3f && confidence_threshold <= 0.8f;
    }

    /// True iff `force_route` holds one of the four legal route tokens (§4.2).
    /// "auto" means "no NS override; let the classifier decide".
    bool IsForceRouteValid() const {
        return force_route == "auto" || force_route == "simple" ||
               force_route == "complex" || force_route == "chat";
    }
};

}  // namespace cortrix::query
