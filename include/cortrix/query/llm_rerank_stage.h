#pragma once
#include <memory>
#include <string>
#include <vector>

#include "cortrix/llm/i_llm_client.h"
#include "cortrix/query/cross_ns_response.h"
#include "cortrix/query/llm_rerank_types.h"

namespace cortrix::query {

/// LlmRerankStage — LLM listwise second-pass reranker (design: hub
/// design/features/F36-llm-listwise-rerank-addendum.md).
///
/// Position in the pipeline: retrieval (+RAG-Fusion variants) → cross-encoder
/// rerank → THIS stage listwise-reranks the head top_n of the response → caller
/// trims back to the requested top_k → CRAG → serialize.
///
/// Contract:
///  - Reorders `resp->results` in place; never fails the query. On any LLM /
///    parse fault the pre-stage order is kept and the response gains
///    meta.warnings[CX_WARN_LLM_RERANK_DEGRADED] (§2.4).
///  - Reordered head scores are rewritten to a monotonically decreasing rank
///    score strictly above the tail's max score, so the "results sorted by
///    score desc" downstream contract holds; the cross-encoder score stays
///    untouched in `rerank_score` for traceability (§2.3).
///  - Prompt carries the same injection defense as the variant generator: random
///    suffix delimiter tags + ignore-instructions rule + strict JSON schema.
class LlmRerankStage {
public:
    /// B/C-class explain state for one Apply() call (§2.5). Returned by value —
    /// the stage itself is stateless across requests.
    struct ExplainState {
        bool active = false;         ///< an LLM listwise call actually ran
        bool degraded = false;       ///< stage could not produce an LLM order → original kept
        bool order_changed = false;  ///< the permutation differs from identity
        int top_n_effective = 0;     ///< min(cfg.top_n, results.size())
        int llm_latency_ms = 0;      ///< summed across windows × consensus votes
        int llm_calls = 0;           ///< total listwise calls issued (§3.5.1/.2)
        int votes_ok = 0;            ///< calls whose ranking was usable
        std::string model_used;
        std::string reason;          ///< "active" / "disabled" / "too_few_results" / "llm_unavailable"
        std::string degrade_reason;  ///< reason-token vocabulary (llm_timeout / ...)
        std::string degrade_detail;  ///< safe short detail (http_status=... / ...)
    };

    /// @param llm  shared OpenAI-compatible client; null → the
    ///             stage degrades every call with reason "llm_unavailable".
    explicit LlmRerankStage(std::shared_ptr<llm::ILlmClient> llm)
        : llm_(std::move(llm)) {}

    bool has_llm() const { return llm_ != nullptr; }

    /// Listwise-rerank the head of `resp->results` against `original_query`.
    /// In-place; see class contract. Total — never throws.
    ExplainState Apply(CrossNsResponse* resp, const std::string& original_query,
                       const LlmRerankConfig& cfg);

    // ---- testing seams (pure helpers) ----

    /// Build the listwise prompt over `presented` (result indices in
    /// presentation order — §3.5.1 consensus varies this order per vote).
    /// Passage [k] in the prompt is resp.results[presented[k-1]].
    static std::string BuildPrompt(const CrossNsResponse& resp,
                                   const std::vector<std::size_t>& presented,
                                   const std::string& original_query,
                                   const LlmRerankConfig& cfg,
                                   const std::string& suffix);

    /// Parse {"ranking":[...]} into a 0-based permutation of [0, n). Tolerates
    /// integer entries, strings carrying an integer ("3", "[3]", "passage 3"),
    /// and objects with an index/id/passage integer key (§3.5.3); drops
    /// out-of-range / duplicate entries; appends missing indices in original
    /// order. Returns false (with `schema_error`) when nothing is usable.
    static bool ParseRankingJson(const std::string& llm_content, std::size_t n,
                                 std::vector<std::size_t>* order_out,
                                 std::string* schema_error);

    /// §3.5.1 presentation order for consensus vote `run` over `n` items:
    /// run 0 = identity (CE order), run 1 = reversed, run 2 = odd-even
    /// interleave. Deterministic (reproducible benchmarks).
    static std::vector<std::size_t> PresentationOrder(std::size_t n, int run);

    /// UTF-8-safe prefix truncation to at most `max_chars` bytes (never splits
    /// a multibyte sequence); newlines collapsed to spaces for [i] list layout.
    static std::string SanitizePassage(const std::string& text, int max_chars);

private:
    std::shared_ptr<llm::ILlmClient> llm_;
};

}  // namespace cortrix::query
