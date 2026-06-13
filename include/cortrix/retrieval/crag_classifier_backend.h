#pragma once
#include <map>
#include <string>

namespace cortrix::retrieval {

/// Output of one CRAG classifier-backend inference (the raw 3-class verdict + its
/// confidence, before CragEvaluator maps it through the configured thresholds).
struct CragBackendResult {
    std::string label;       ///< "correct" / "ambiguous" / "incorrect"
    float score = 0.0f;      ///< verdict score in [0,1] (softmax confidence of the chosen class)
    float confidence = 0.0f; ///< classifier self-confidence; < config.classifier_min_confidence → heuristic guard
};

/// ICragClassifierBackend — the pluggable inference backend behind CragEvaluator
/// (F37 §5.2 / §6.2 RunClassifier).
///
/// WHY THIS INTERFACE (standalone / Lead-approved injection point): F37 §5.2's
/// CragEvaluator ctor names `std::shared_ptr<OnnxSession>`, but `OnnxSession` is a
/// stale name — no such type exists in the frozen tree (onnx/ only exposes the
/// static cortrix::onnx::Runtime version/opset introspector; F02 holds its own raw
/// Ort::Session). Per the C-R1 briefing the real DistilBERT-tiny inference is
/// D3.5-deferred and standalone uses a heuristic-guard stub. So CragEvaluator
/// depends on THIS small interface instead of a non-existent ONNX session:
///   - HeuristicGuardBackend (this round): no ONNX dependency, fully testable.
///   - OnnxCragBackend (D3.5): real DistilBERT-tiny via cortrix::onnx::Runtime.
/// The interface keeps CragEvaluator's API stable across that swap.
class ICragClassifierBackend {
public:
    virtual ~ICragClassifierBackend() = default;

    /// Infer a verdict from the query, the top-1 chunk text (§6.2 uses only
    /// chunks[0].chunk_text as the model's text input), and the precomputed
    /// multi-signal feature map (top1 / median / std / high_score_ratio / ...).
    /// Must be total: a backend fault is signalled by throwing CragInferenceError
    /// (caught by CragEvaluator's L3 retry/degrade path), never by a partial result.
    virtual CragBackendResult Infer(
        const std::string& query,
        const std::string& top_chunk_text,
        const std::map<std::string, float>& signals) = 0;

    /// Stable backend name (logging / debug). e.g. "heuristic_guard" / "onnx_distilbert_tiny".
    virtual std::string Name() const = 0;

    /// True when the backend is loaded and usable (drives L1/L2 skip + IsAvailable()).
    virtual bool IsAvailable() const = 0;
};

}  // namespace cortrix::retrieval
