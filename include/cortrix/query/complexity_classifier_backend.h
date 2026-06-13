#pragma once
#include <string>

namespace cortrix::query {

/// Output of one complexity classifier-backend inference: the raw 3-class label +
/// its softmax confidence, before QueryComplexityClassifier applies the
/// confidence-threshold fail-safe (F39 §6.1 step 5).
struct ComplexityBackendResult {
    std::string label;       ///< "simple" / "complex" / "chat"
    float confidence = 0.0f; ///< softmax confidence of the chosen class, in [0,1]
};

/// IComplexityClassifierBackend — the pluggable inference backend behind
/// QueryComplexityClassifier (F39 §5.1 RunClassifier).
///
/// WHY THIS INTERFACE (standalone / mirrors F37's ICragClassifierBackend): F39
/// §5.1's QueryComplexityClassifier ctor names `std::shared_ptr<OnnxSession>`, but
/// `OnnxSession` is a stale name — no such type exists in the frozen tree (onnx/
/// only exposes the static cortrix::onnx::Runtime version/opset introspector; F02
/// holds its own raw Ort::Session). Per the C-R2 briefing the real DistilBERT-tiny
/// inference is D3.5-deferred and standalone uses a heuristic-guard stub. So
/// QueryComplexityClassifier depends on THIS small interface instead of a
/// non-existent ONNX session:
///   - HeuristicComplexityBackend (this round): no ONNX dependency, fully testable.
///   - OnnxComplexityBackend (D3.5): real DistilBERT-tiny via cortrix::onnx::Runtime.
/// The interface keeps QueryComplexityClassifier's API stable across that swap.
/// This exactly follows F37's already-merged ICragClassifierBackend pattern.
class IComplexityClassifierBackend {
public:
    virtual ~IComplexityClassifierBackend() = default;

    /// Infer a 3-class verdict for `query` (the model's only text input, §6.1).
    /// Must be total: a transient backend fault is signalled by throwing
    /// RouterInferenceError (caught by QueryComplexityClassifier's L3 retry/degrade
    /// path), never by a partial result. No other exception may escape.
    virtual ComplexityBackendResult Infer(const std::string& query) = 0;

    /// Stable backend name (logging / debug). e.g. "heuristic_guard" /
    /// "onnx_distilbert_tiny".
    virtual std::string Name() const = 0;

    /// True when the backend is loaded and usable (drives L2 skip + IsAvailable()).
    virtual bool IsAvailable() const = 0;
};

}  // namespace cortrix::query
