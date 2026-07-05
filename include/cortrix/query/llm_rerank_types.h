#pragma once
#include <cstdint>
#include <string>

namespace cortrix::query {

/// F36-LR: request-scoped config for the LLM listwise rerank stage (design:
/// hub design/features/F36-llm-listwise-rerank-addendum.md §2.1).
///
/// Unlike F36 RagFusionConfig (query expansion — candidate generation), this
/// stage lets the LLM participate in ORDERING: it listwise-reranks the top_n
/// candidates that retrieval + the F02 cross-encoder already ranked. Default
/// disabled (cost + latency); an Agent opts in per request via `llm_rerank`
/// (body/param) or `llm_rerank_config` (body object).
struct LlmRerankConfig {
    bool enabled = false;        ///< default off (product safety §2)
    int top_n = 20;              ///< listwise window; wiring expands retrieval to it
    int max_doc_chars = 600;     ///< per-passage prompt truncation (UTF-8 safe)
    int64_t timeout_ms = 45000;  ///< listwise LLM call timeout
    std::string model;           ///< per-call model override; empty = client default
    std::string locale = "en";   ///< prompt locale: "en" (default) or "zh"
};

/// Inclusive validation bounds (§2.1).
constexpr int kLlmRerankTopNMin = 2;
constexpr int kLlmRerankTopNMax = 50;
constexpr int kLlmRerankMaxDocCharsMin = 100;
constexpr int kLlmRerankMaxDocCharsMax = 4000;
constexpr int64_t kLlmRerankTimeoutMsMin = 1000;
constexpr int64_t kLlmRerankTimeoutMsMax = 120000;

/// Validate against the §2.1 ranges. Returns false (and fills `field` /
/// `valid_range` for the CX_ERR_LLM_RERANK_CONFIG_INVALID body) on the first
/// out-of-range field; true when all fields are in range.
bool ValidateLlmRerankConfig(const LlmRerankConfig& cfg,
                             std::string* field = nullptr,
                             std::string* valid_range = nullptr);

}  // namespace cortrix::query
