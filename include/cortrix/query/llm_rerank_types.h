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
    /// §3.5.1 presentation-order consensus votes per window. GLM at t=0 is
    /// deterministic, so extra votes vary the PRESENTATION order (identity /
    /// reversed / odd-even interleave) and Borda-aggregate — this debiases the
    /// known listwise position bias instead of re-sampling the same answer.
    int consensus_runs = 1;
};

/// Inclusive validation bounds (§2.1 / §3.5.1).
constexpr int kLlmRerankTopNMin = 2;
/// 50 → 200 (2026-07-12): deep-window probing. The 5k separation probe put
/// recall@200 at 0.9024 vs 0.8318 within the top-50 window — the remaining
/// headroom lives at ranks 51-200, unreachable under the old cap. The wiring
/// already widens retrieval top_k to top_n and trims back after the stage, so
/// the cap was the only blocker. Default top_n (20) is unchanged; cost scales
/// linearly with top_n and remains the Agent's per-request opt-in.
constexpr int kLlmRerankTopNMax = 200;
constexpr int kLlmRerankMaxDocCharsMin = 100;
constexpr int kLlmRerankMaxDocCharsMax = 4000;
constexpr int64_t kLlmRerankTimeoutMsMin = 1000;
constexpr int64_t kLlmRerankTimeoutMsMax = 120000;
constexpr int kLlmRerankConsensusRunsMin = 1;
constexpr int kLlmRerankConsensusRunsMax = 3;

/// §3.5.2 sliding-window geometry for top_n > kLlmRerankWindowSize (RankGPT
/// bottom-up: rank the tail window first so strong tail candidates bubble
/// upward through the overlap into the head window).
constexpr int kLlmRerankWindowSize = 20;
constexpr int kLlmRerankWindowStride = 10;

/// Validate against the §2.1 ranges. Returns false (and fills `field` /
/// `valid_range` for the CX_ERR_LLM_RERANK_CONFIG_INVALID body) on the first
/// out-of-range field; true when all fields are in range.
bool ValidateLlmRerankConfig(const LlmRerankConfig& cfg,
                             std::string* field = nullptr,
                             std::string* valid_range = nullptr);

}  // namespace cortrix::query
