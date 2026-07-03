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
    // At least one strategy when enabled (an empty strategy list would generate
    // nothing). Allowed to be empty when disabled (config is inert then).
    if (cfg.enabled && cfg.variant_strategies.empty()) {
        return fail("variant_strategies", "non-empty when enabled");
    }
    return true;
}

}  // namespace cortrix::query
