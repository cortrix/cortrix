#pragma once
#include <string>

#include "cortrix/query/complexity_classifier_backend.h"
#include "cortrix/query/wordpiece_tokenizer.h"

namespace cortrix::query {

/// OnnxComplexityBackend — the D3.5 real DistilBERT inference backend behind
/// QueryComplexityClassifier. It replaces the standalone
/// HeuristicComplexityBackend behind the same IComplexityClassifierBackend
/// interface, so QueryComplexityClassifier (and its rule-guard / force-route /
/// confidence-fail-safe logic) is unchanged by the swap.
///
/// Model: `models/query-complexity/model.onnx` — a fine-tuned
/// `distilbert-base-uncased` 3-class sequence classifier (model README).
///   inputs : input_ids [batch, seq] int64, attention_mask [batch, seq] int64
///   output : probs [batch, 3] float — softmax IS applied in-graph; argmax → the
///            label id, max value → confidence. Label order (README):
///                0 = chat, 1 = simple, 2 = complex.
/// Tokenizer: WordPieceTokenizer over `vocab.txt` (distilbert-base-uncased),
/// max_len 64 (README training config truncates to 64).
///
/// Ort handling mirrors the in-tree OnnxEmbedder / OnnxReranker pattern: Ort types
/// are kept out of this header (opaque void* env_/session_) so non-ONNX TUs may
/// include it, and the session is created in Init().
///
/// Availability / failure model (mapped from the embedder's pattern):
///   - Model file absent OR ONNX Runtime not linked → Init() returns Ok but leaves
///     the backend UNAVAILABLE (IsAvailable()==false). QueryComplexityClassifier
///     then takes the L2 "classifier_unavailable" default-Complex path. This keeps
///     CI / offline builds green without a 256 MB model.
///   - A model present but failing to load (corrupt / opset / tokenizer) → Init()
///     returns the underlying error AND leaves the backend unavailable. The ONNX
///     StartupValidator hard-fails the process on a registered model that does not
///     validate; once past that gate a load failure here is the L2 path.
///   - A transient *inference* fault at query time → Infer() retries once then
///     throws RouterInferenceError, which QueryComplexityClassifier catches for its
///     L3 retry/degrade-to-Complex path. No other exception escapes Infer().
class OnnxComplexityBackend : public IComplexityClassifierBackend {
public:
    /// @param model_path     path to model.onnx (e.g. models/query-complexity/model.onnx).
    /// @param vocab_path     path to vocab.txt; defaults (empty) to `vocab.txt`
    ///                       next to model_path.
    /// @param max_seq_length tokenizer truncation length (README = 64).
    /// @param intra_threads  ONNX Runtime intra-op threads (0 = auto, like OnnxEmbedder).
    explicit OnnxComplexityBackend(std::string model_path,
                                   std::string vocab_path = "",
                                   int max_seq_length = 64,
                                   int intra_threads = 1);
    ~OnnxComplexityBackend() override;

    OnnxComplexityBackend(const OnnxComplexityBackend&) = delete;
    OnnxComplexityBackend& operator=(const OnnxComplexityBackend&) = delete;

    /// Load the WordPiece vocab + create the ONNX Session. Returns Ok and leaves
    /// the backend unavailable when the model file is absent / ONNX is not linked
    /// (offline-friendly); returns the underlying error (and stays unavailable)
    /// when a present model/vocab fails to load. Idempotent: a second call is a
    /// no-op once loaded.
    Status Init();

    // --- IComplexityClassifierBackend ---

    /// Tokenize `query`, run the model, map argmax(probs) → {chat,simple,complex}
    /// with the max-prob confidence. Throws RouterInferenceError after one retry on
    /// a transient ONNX fault (caught by QueryComplexityClassifier's L3 path).
    /// Precondition: IsAvailable(); calling it otherwise throws RouterInferenceError
    /// (the classifier never does — it checks IsAvailable() / L2 first).
    ComplexityBackendResult Infer(const std::string& query) override;

    std::string Name() const override { return "onnx_distilbert"; }
    bool IsAvailable() const override { return ready_; }

private:
    std::string model_path_;
    std::string vocab_path_;
    int max_seq_length_;
    int intra_threads_;

    bool ready_ = false;       ///< true once vocab + session both loaded
    WordPieceTokenizer tokenizer_;

    // Opaque Ort handles (actual types in the .cpp under CORTRIX_HAS_ONNXRUNTIME).
    void* env_ = nullptr;      ///< Ort::Env*
    void* session_ = nullptr;  ///< Ort::Session*
};

}  // namespace cortrix::query
