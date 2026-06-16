#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/config/config.h"
#include "cortrix/retrieval/classifier.h"  // IClassifier / ClassifierInput / ClassificationResult (S1)

namespace cortrix {

/// Intent classification result
enum class QueryIntent {
    kSemantic = 0,    // vector + BM25 only
    kSql      = 1,    // vector + BM25 + SQL
    kHybrid   = 2,    // all three routes
};

/// IntentClassifier — classifies a query into a retrieval intent (semantic / sql /
/// hybrid). The query-engine's existing intent gate.
///
/// S2 ruling (G3+G1.2 joint D1, decision D / backward-compatible): IntentClassifier
/// is adapted to be a `cortrix::retrieval::IClassifier` instance so the query
/// pipeline can treat all classifiers (F37 CRAG, F39 complexity, intent) uniformly.
/// This is a *pure addition* — the existing `Classify(query_text, timeout_us)`
/// signature and all its call sites are unchanged (zero break). The new
/// IClassifier::Classify(ClassifierInput) overload is a thin adapter over the same
/// keyword/LLM logic; `input.chunks` is ignored (intent is a pre-retrieval signal).
class IntentClassifier : public retrieval::IClassifier {
public:
    /// @param llm_config: LLM configuration (provider/api_key/model)
    ///        If provider is empty, auto-degrades to keyword mode
    explicit IntentClassifier(const LlmConfig& llm_config);

    /// Classify query intent (existing API — UNCHANGED, S2 backward compatibility).
    /// LLM mode: call LLM API for three-way classification
    /// Fallback mode: keyword rule matching
    ///
    /// @param query_text: raw query text
    /// @param timeout_us: classification timeout (microseconds)
    /// @return QueryIntent
    QueryIntent Classify(const std::string& query_text, int64_t timeout_us);

    // --- IClassifier (S2 shared contract; additive) ---
    /// Adapter over the intent logic: returns label "semantic"/"sql"/"hybrid" with
    /// confidence 1.0 (keyword/LLM intent has no calibrated score in Phase 1).
    /// Total (never throws): the underlying Classify already degrades to keyword
    /// matching on any LLM fault. `input.chunks` is ignored.
    retrieval::ClassificationResult Classify(
        const retrieval::ClassifierInput& input) override;
    std::string Name() const override { return "intent-classifier"; }
    std::vector<std::string> Labels() const override {
        return {"semantic", "sql", "hybrid"};
    }
    /// Always usable: keyword matching is always available even with no LLM config.
    bool IsAvailable() const override { return true; }

    /// Whether LLM mode is active
    bool IsLlmEnabled() const;

    /// Stable string label for a QueryIntent (S2 IClassifier::Labels() vocabulary).
    static const char* IntentToLabel(QueryIntent intent);

private:
    /// LLM API classification
    QueryIntent ClassifyByLlm(const std::string& query_text, int64_t timeout_us);

    /// Keyword rule classification (fallback when LLM unavailable)
    static QueryIntent ClassifyByKeyword(const std::string& query_text);

    LlmConfig llm_config_;
    bool llm_enabled_ = false;
};

}  // namespace cortrix
