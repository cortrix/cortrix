#pragma once
#include <map>
#include <string>

#include "cortrix/retrieval/crag_classifier_backend.h"

namespace cortrix::retrieval {

/// HeuristicGuardBackend — the standalone (no-ONNX) CRAG inference backend
/// (C-R1 briefing: real DistilBERT-tiny inference is D3.5-deferred; standalone
/// uses a heuristic guard stub). It derives a verdict score purely from the
/// precomputed multi-signals, so CragEvaluator is fully exercisable + testable
/// without any model. In D3.5 this is swapped for OnnxCragBackend behind the same
/// ICragClassifierBackend interface.
///
/// Scoring: a weighted blend of top1 (dominant relevance) and the high-score ratio
/// (breadth of agreement), matching CragEvaluator::HeuristicGuard. Reports a
/// configurable confidence (default 1.0) so the evaluator takes the main backend
/// path rather than the internal low-confidence guard — tests inject a lower value
/// to exercise the guard fallback.
class HeuristicGuardBackend : public ICragClassifierBackend {
public:
    explicit HeuristicGuardBackend(float reported_confidence = 1.0f)
        : reported_confidence_(reported_confidence) {}

    CragBackendResult Infer(const std::string& /*query*/,
                            const std::string& /*top_chunk_text*/,
                            const std::map<std::string, float>& signals) override {
        auto get = [&signals](const char* k) -> float {
            auto it = signals.find(k);
            return it == signals.end() ? 0.0f : it->second;
        };
        const float top1 = get("top1");
        const float high_ratio = get("high_score_ratio");
        float score = 0.7f * top1 + 0.3f * high_ratio;
        if (score < 0.0f) score = 0.0f;
        if (score > 1.0f) score = 1.0f;

        CragBackendResult r;
        r.score = score;
        r.label = "";  // empty → CragEvaluator derives the label from the score via NS thresholds
        r.confidence = reported_confidence_;
        return r;
    }

    std::string Name() const override { return "heuristic_guard"; }
    bool IsAvailable() const override { return true; }

private:
    float reported_confidence_;
};

}  // namespace cortrix::retrieval
