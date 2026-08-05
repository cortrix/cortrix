#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cortrix::query {

/// Variant generation strategy (3 locked strategies).
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

/// NS-level RAG-Fusion config. Persisted as the
/// namespaces.rag_fusion_config JSONB blob (the 7th *_config field) and
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
    std::string locale = "zh";                              ///< prompt locale: "zh" (default) or "en"
    std::string model;                                      ///< §3.5.4 per-call variant-LLM override; empty = client default
    int candidate_multiplier = 1;                            ///< v1.0.9: per-variant top_k multiplier before outer fusion
    int max_candidates = 50;                                 ///< v1.0.9: cap for expanded per-variant candidate pool
    bool final_rerank = false;                               ///< v1.0.9: optional final rerank after outer fusion
    std::string activation_policy = "always";                ///< v1.0.11: "always" or "selective_margin"
    float activation_score_margin = 0.0f;                    ///< v1.0.11: skip LLM when original top1-top2 margin >= this
    int activation_min_results = 2;                           ///< v1.0.11: need at least this many original results to skip
    /// v1.0.13 (§4.3.bis.5): the anchored-fusion policy constants, previously
    /// hardcoded (1.7 / 0.5 / 3). The fixed policy proved scale-sensitive: at
    /// 5000-doc FiQA, variant evidence at 0.5×N displaces original rank-4..10
    /// strong hits (identical candidate pools, -10.8pp recall@10 vs listwise).
    /// Defaults are byte-identical to the v1.0.7 behavior.
    float fusion_original_weight = 1.7f;                      ///< v1.0.13: outer-RRF original-query role weight
    float fusion_variant_weight = 0.5f;                       ///< v1.0.13: outer-RRF LLM-variant role weight (0 = variants only fill the pool)
    int fusion_anchor_max = 3;                                ///< v1.0.13: original head anchor cap [0-10]; 0 = no anchor segment
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RagFusionConfig, enabled, variant_count,
                                   variant_strategies, rrf_k, timeout_ms, locale,
                                   model, candidate_multiplier, max_candidates,
                                   final_rerank, activation_policy,
                                   activation_score_margin, activation_min_results,
                                   fusion_original_weight, fusion_variant_weight,
                                   fusion_anchor_max)

/// The inclusive bounds for variant_count (topic 1: NS-tunable [1-10]).
constexpr int kVariantCountMin = 1;
constexpr int kVariantCountMax = 10;
constexpr int kRagFusionCandidateMultiplierMin = 1;
constexpr int kRagFusionCandidateMultiplierMax = 5;
constexpr int kRagFusionMaxCandidatesMin = 1;
constexpr int kRagFusionMaxCandidatesMax = 200;
constexpr int kRagFusionActivationMinResultsMin = 2;
constexpr int kRagFusionActivationMinResultsMax = 200;
constexpr float kRagFusionActivationScoreMarginMin = 0.0f;
constexpr float kRagFusionActivationScoreMarginMax = 10.0f;
constexpr float kRagFusionOriginalWeightMin = 0.1f;   ///< v1.0.13: original must stay a positive signal
constexpr float kRagFusionVariantWeightMin = 0.0f;    ///< v1.0.13: 0 = pool-membership only
constexpr float kRagFusionFusionWeightMax = 10.0f;
constexpr int kRagFusionAnchorMaxMin = 0;             ///< v1.0.13: 0 disables the anchor segment
constexpr int kRagFusionAnchorMaxMax = 10;

/// Validate a config's fields against the §4.4 / §7 ranges. Returns false (and
/// fills `field`/`valid_range` for a CX_ERR_RAG_FUSION_CONFIG_INVALID body) on the
/// first out-of-range field; true if all fields are in range.
bool ValidateRagFusionConfig(const RagFusionConfig& cfg,
                             std::string* field = nullptr,
                             std::string* valid_range = nullptr);

/// Variant generation result. `variants` does NOT include the original
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
