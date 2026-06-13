#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cortrix::query {

/// Variant generation strategy (F36 §4.1 / topic D5 — ARCH locked 3 strategies).
/// Serializes to the lowercase string the LLM prompt + NS config JSON use.
enum class VariantStrategy {
    kParaphrase,   ///< "paraphrase" — synonym rewrite
    kSubquery,     ///< "subquery"   — decompose into sub-questions
    kReverse,      ///< "reverse"    — reverse phrasing
};

/// "paraphrase" / "subquery" / "reverse" for `strategy`.
const char* VariantStrategyString(VariantStrategy s);

/// Parse a strategy string; returns false (and leaves `out` untouched) on an
/// unknown value so callers can decide how to handle a malformed LLM/NS value.
bool ParseVariantStrategy(const std::string& s, VariantStrategy* out);

// nlohmann (de)serialization for VariantStrategy — used by RagFusionConfig
// (de)serialization and the ConfigResolver<RagFusionConfig> three-layer merge
// (§4.4). An unknown string deserializes to kParaphrase (defensive: never throws,
// so a malformed NS blob can't crash the query path; ConfigResolver already
// validates the object shape).
inline void to_json(nlohmann::json& j, const VariantStrategy& s) {
    j = VariantStrategyString(s);
}
inline void from_json(const nlohmann::json& j, VariantStrategy& s) {
    VariantStrategy parsed = VariantStrategy::kParaphrase;
    if (j.is_string()) ParseVariantStrategy(j.get<std::string>(), &parsed);
    s = parsed;
}

/// NS-level RAG-Fusion config (F36 §4.1 / §4.4). Persisted as the
/// namespaces.rag_fusion_config JSONB blob (F12 v1.0.4 7th *_config field) and
/// resolved global ← NS ← request via ConfigResolver<RagFusionConfig> (§4.4).
///
/// Defaults encode topic 1 (N=3) + topic 3 (V1.0 OSS default disabled).
struct RagFusionConfig {
    bool enabled = false;                                   ///< topic 3: V1.0 OSS default false
    int variant_count = 3;                                  ///< topic 1: N=3 (NS-tunable [1-10])
    std::vector<VariantStrategy> variant_strategies = {     ///< topic D5: ARCH locked 3
        VariantStrategy::kParaphrase,
        VariantStrategy::kSubquery,
        VariantStrategy::kReverse,
    };
    int rrf_k = 60;                                         ///< sentinel ARCH RRFFusion default
    int64_t timeout_ms = 5000;                              ///< LLM call timeout (topic 4)
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RagFusionConfig, enabled, variant_count,
                                   variant_strategies, rrf_k, timeout_ms)

/// The inclusive bounds for variant_count (topic 1: NS-tunable [1-10]).
constexpr int kVariantCountMin = 1;
constexpr int kVariantCountMax = 10;

/// Validate a config's fields against the §4.4 / §7 ranges. Returns false (and
/// fills `field`/`valid_range` for a CX_ERR_RAG_FUSION_CONFIG_INVALID body) on the
/// first out-of-range field; true if all fields are in range.
bool ValidateRagFusionConfig(const RagFusionConfig& cfg,
                             std::string* field = nullptr,
                             std::string* valid_range = nullptr);

/// Variant generation result (F36 §4.1). `variants` does NOT include the original
/// query — the caller merges it (§4.5 `all_queries = {query} + variants`).
struct QueryVariants {
    std::string original_query;
    std::vector<std::string> variants;   ///< excludes the original query
    int llm_latency_ms = 0;
    std::string llm_model_used;          ///< B-class — only via ?explain=true
    std::string llm_prompt_version;      ///< B-class — only via ?explain=true
    int64_t token_count = 0;             ///< B-class — only via ?explain=true
};

}  // namespace cortrix::query
