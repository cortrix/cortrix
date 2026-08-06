#pragma once
#include <string>

namespace cortrix::query {

/// ComplexityConfig — resolved configuration for the query-complexity router
/// (NS metadata.complexity_config + ctor). Phase 1: the effective
/// config is the fixed defaults below, optionally overridden per-namespace by the
/// JSONB `complexity_config` column (the catalog NS-config resolution path is
/// cross-Feature wiring → integration; this struct is the in-memory shape both the
/// defaults and any future resolver fill).
///
/// Field semantics map 1:1 to the schema:
///   enabled                         — L1 NS-config path switch (default on / zero-config)
///   force_route                     — "auto" / "simple" / "complex" / "chat" (NS-level override)
///   confidence_threshold            — step 5: classifier confidence below this → Complex (fail-safe)
///   evaluation_method               — "small_classifier" / "hybrid" (Phase 2 LLM judge)
///   fallback_to_complex_on_failure  — L1/L2/L3 default behavior
///   multi_turn_warning_enabled      — multi_turn_context_warning detection toggle
///   max_inference_retries           — L3 retry budget (exponential back-off 50/100/200ms)
struct ComplexityConfig {
    bool enabled = true;                       ///< L1 NS-config path switch
    std::string force_route = "auto";          ///< "auto"/"simple"/"complex"/"chat"
    float confidence_threshold = 0.5f;         ///< NS-configurable [0.3, 0.8]
    std::string evaluation_method = "small_classifier";  ///< classifier evaluation method
    bool fallback_to_complex_on_failure = true;          ///< fail-safe default
    bool multi_turn_warning_enabled = true;    ///< multi_turn_context_warning toggle
    int max_inference_retries = 3;             ///< L3 retry budget

    /// Boundary hard-limit: a NS-supplied confidence_threshold is valid
    /// only inside [0.3, 0.8]. Callers raise CX_ERR_ROUTER_FORCE_ROUTE_INVALID's sibling
    /// validation at config-resolution time (integration); the router clamps defensively.
    bool IsThresholdValid() const {
        return confidence_threshold >= 0.3f && confidence_threshold <= 0.8f;
    }

    /// True iff `force_route` holds one of the four legal route tokens.
    /// "auto" means "no NS override; let the classifier decide".
    bool IsForceRouteValid() const {
        return force_route == "auto" || force_route == "simple" ||
               force_route == "complex" || force_route == "chat";
    }
};

}  // namespace cortrix::query
