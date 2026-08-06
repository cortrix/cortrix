#pragma once
#include <string>

#include "cortrix/query/complexity_classifier_backend.h"

namespace cortrix::query {

/// HeuristicComplexityBackend — the standalone (no-ONNX) complexity inference
/// backend (C-R2 briefing: real DistilBERT-tiny inference is integration-deferred;
/// standalone uses a heuristic guard stub). It derives a Simple/Complex/Chat label
/// purely from cheap surface features of the query text (length in whitespace
/// tokens + presence of multi-hop / comparison cue words), so
/// QueryComplexityClassifier is fully exercisable + testable without any model. In
/// integration this is swapped for OnnxComplexityBackend behind the same
/// IComplexityClassifierBackend interface. This mirrors HeuristicGuardBackend.
///
/// Heuristic (Adaptive-RAG label intuition NQ/HotpotQA/SQuAD2.0):
///   - very short / no real content → "chat" (Chat detection is handled earlier by
///     the classifier's IsChatQuery rule guard; this backend's chat branch is a
///     safety net for degenerate input the rule guard let through).
///   - contains a multi-hop / comparison cue ("compare", "vs", "difference
///     between", or a CJK equivalent …) OR is long → "complex" (multi-step).
///   - otherwise → "simple" (single-step).
///
/// Reports a configurable confidence (default 0.9) so the evaluator takes the main
/// backend path rather than the low-confidence fail-safe — tests inject a lower
/// value to exercise that fallback.
class HeuristicComplexityBackend : public IComplexityClassifierBackend {
public:
    explicit HeuristicComplexityBackend(float reported_confidence = 0.9f)
        : reported_confidence_(reported_confidence) {}

    ComplexityBackendResult Infer(const std::string& query) override {
        ComplexityBackendResult r;
        r.confidence = reported_confidence_;

        const std::string trimmed = Trim(query);
        if (trimmed.empty()) {
            r.label = "chat";  // safety net: empty content has nothing to retrieve
            return r;
        }

        if (HasMultiHopCue(query) || WordCount(trimmed) >= kComplexWordThreshold) {
            r.label = "complex";
        } else {
            r.label = "simple";
        }
        return r;
    }

    std::string Name() const override { return "heuristic_guard"; }
    bool IsAvailable() const override { return true; }

private:
    // A query with at least this many whitespace-separated tokens is treated as
    // multi-step (Complex). Kept small + documented so the standalone behavior is
    // predictable for tests; the real classifier replaces this in integration.
    static constexpr int kComplexWordThreshold = 12;

    static std::string Trim(const std::string& s) {
        size_t b = 0;
        size_t e = s.size();
        while (b < e && IsSpace(s[b])) ++b;
        while (e > b && IsSpace(s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    static bool IsSpace(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }

    static int WordCount(const std::string& s) {
        int count = 0;
        bool in_word = false;
        for (char c : s) {
            if (IsSpace(c)) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                ++count;
            }
        }
        return count;
    }

    // Multi-hop / comparison cue words drive a Complex verdict. English cues are
    // matched case-insensitively; the multi-byte entries are Chinese comparison
    // phrases (functional matching data, kept as UTF-8 bytes — not display text).
    static bool HasMultiHopCue(const std::string& query) {
        std::string lower;
        lower.reserve(query.size());
        for (char c : query) {
            lower.push_back(static_cast<char>(
                (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
        }
        static const char* en_cues[] = {
            "compare", " vs ", "versus", "difference between", "differences between",
            "trade-off", "tradeoff", "pros and cons", "how does", "why does",
            "relationship between", "impact of",
        };
        for (const char* cue : en_cues) {
            if (lower.find(cue) != std::string::npos) return true;
        }
        // Chinese comparison / multi-hop cues (compare / difference / compared-to /
        // why), matched on raw UTF-8 bytes in cn_cues below.
        static const char* cn_cues[] = {
            "\xe5\xaf\xb9\xe6\xaf\x94",        // "compare"
            "\xe5\x8c\xba\xe5\x88\xab",        // "difference"
            "\xe7\x9b\xb8\xe6\xaf\x94",        // "compared to"
            "\xe4\xb8\xba\xe4\xbb\x80\xe4\xb9\x88",  // "why"
        };
        for (const char* cue : cn_cues) {
            if (query.find(cue) != std::string::npos) return true;
        }
        return false;
    }

    float reported_confidence_;
};

}  // namespace cortrix::query
