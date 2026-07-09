#include "cortrix/query/rag_fusion_types.h"

namespace cortrix::query {

const char* VariantStrategyString(VariantStrategy s) {
    switch (s) {
        case VariantStrategy::kParaphrase: return "paraphrase";
        case VariantStrategy::kSubquery:   return "subquery";
        case VariantStrategy::kReverse:    return "reverse";
    }
    return "paraphrase";  // unreachable for a valid enum
}

bool ParseVariantStrategy(const std::string& s, VariantStrategy* out) {
    if (s == "paraphrase") { if (out) *out = VariantStrategy::kParaphrase; return true; }
    if (s == "subquery")   { if (out) *out = VariantStrategy::kSubquery;   return true; }
    if (s == "reverse")    { if (out) *out = VariantStrategy::kReverse;    return true; }
    return false;
}

bool ValidateRagFusionConfig(const RagFusionConfig& cfg,
                             std::string* field,
                             std::string* valid_range) {
    auto fail = [&](const char* f, const char* r) {
        if (field) *field = f;
        if (valid_range) *valid_range = r;
        return false;
    };
    // topic 1: variant_count in [1, 10].
    if (cfg.variant_count < kVariantCountMin || cfg.variant_count > kVariantCountMax) {
        return fail("variant_count", "[1, 10]");
    }
    // rrf_k must be positive (RRF formula 1/(k+rank) needs k > 0).
    if (cfg.rrf_k <= 0) {
        return fail("rrf_k", "[1, ]");
    }
    // timeout must be positive.
    if (cfg.timeout_ms <= 0) {
        return fail("timeout_ms", "[1, ]");
    }
    if (cfg.locale != "zh" && cfg.locale != "en") {
        return fail("locale", "zh|en");
    }
    if (cfg.candidate_multiplier < kRagFusionCandidateMultiplierMin ||
        cfg.candidate_multiplier > kRagFusionCandidateMultiplierMax) {
        return fail("candidate_multiplier", "[1, 5]");
    }
    if (cfg.max_candidates < kRagFusionMaxCandidatesMin ||
        cfg.max_candidates > kRagFusionMaxCandidatesMax) {
        return fail("max_candidates", "[1, 200]");
    }
    if (cfg.activation_policy != "always" &&
        cfg.activation_policy != "selective_margin") {
        return fail("activation_policy", "always|selective_margin");
    }
    if (cfg.activation_score_margin < kRagFusionActivationScoreMarginMin ||
        cfg.activation_score_margin > kRagFusionActivationScoreMarginMax) {
        return fail("activation_score_margin", "[0, 10]");
    }
    if (cfg.activation_min_results < kRagFusionActivationMinResultsMin ||
        cfg.activation_min_results > kRagFusionActivationMinResultsMax) {
        return fail("activation_min_results", "[2, 200]");
    }
    // v1.0.13 fusion-policy knobs (§4.3.bis.5).
    if (cfg.fusion_original_weight < kRagFusionOriginalWeightMin ||
        cfg.fusion_original_weight > kRagFusionFusionWeightMax) {
        return fail("fusion_original_weight", "[0.1, 10]");
    }
    if (cfg.fusion_variant_weight < kRagFusionVariantWeightMin ||
        cfg.fusion_variant_weight > kRagFusionFusionWeightMax) {
        return fail("fusion_variant_weight", "[0, 10]");
    }
    if (cfg.fusion_anchor_max < kRagFusionAnchorMaxMin ||
        cfg.fusion_anchor_max > kRagFusionAnchorMaxMax) {
        return fail("fusion_anchor_max", "[0, 10]");
    }
    // At least one strategy when enabled (an empty strategy list would generate
    // nothing). Allowed to be empty when disabled (config is inert then).
    if (cfg.enabled && cfg.variant_strategies.empty()) {
        return fail("variant_strategies", "non-empty when enabled");
    }
    return true;
}

}  // namespace cortrix::query
