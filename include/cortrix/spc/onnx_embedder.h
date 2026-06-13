#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "cortrix/common/status.h"

namespace cortrix {

struct EmbeddingResult {
    std::vector<float> vector;
    int dim = 0;
    float inference_time_ms = 0.0f;
};

/// Dense + sparse pair from a single BGE-M3 forward pass (F40 §5.1, F40-1 A:
/// "write-time single inference outputs dense + sparse"). `dense` is the same
/// 1024-dim CLS embedding EmbedBatch produces; `sparse` is the SPLADE-style
/// lexical_weights map (BGE-M3 vocab token_id → activation weight), top-K
/// truncated by the caller (default K=100, F40-6). An empty `sparse` map is a
/// valid result — it means the chunk produced no active sparse terms (a "dead"
/// chunk, F40 §6.5) and the caller writes sparse_vec=NULL + clears the F09
/// flags_ext has_sparse_vec bit.
struct EmbedWithSparseResult {
    std::vector<float> dense;          ///< 1024-dim dense embedding (unit-normalized)
    std::map<uint32_t, float> sparse;  ///< lexical_weights: term_id → weight
    float inference_time_ms = 0.0f;
};

/// ONNX Runtime embedder for bge-m3 (1024-dim)
class OnnxEmbedder {
public:
    /// @param model_path: path to model.onnx
    /// @param dim: expected embedding dimension
    /// @param intra_threads: ONNX Runtime intra-op threads
    /// @param inter_threads: ONNX Runtime inter-op threads
    OnnxEmbedder(const std::string& model_path, int dim = 1024,
                 int intra_threads = 4, int inter_threads = 1);
    ~OnnxEmbedder();

    /// Initialize ONNX Runtime session
    Status Init();

    /// Embed a single text
    Status Embed(const std::string& text, EmbeddingResult* result);

    /// Batch embed multiple texts
    Status EmbedBatch(const std::vector<std::string>& texts,
                      std::vector<EmbeddingResult>* results);

    /// ✨ F40 S1 (F40-1 decision A) — single BGE-M3 forward pass producing both
    /// the dense embedding and the SPLADE sparse lexical_weights. `top_k` caps
    /// the sparse map to its top-`top_k` weights (F40-6 K=100 default,
    /// NS-configurable 50-200); top_k<=0 keeps every active term.
    ///
    /// Real sparse-head extraction from a re-exported BGE-M3 ONNX model that
    /// exposes the lexical_weights output is cross-Feature wiring (model export
    /// + a 2nd model output) → D3.5. In standalone D3 the dense half reuses the
    /// frozen real inference path (F22), and the sparse half is produced by a
    /// deterministic stub derived from the same input (so the serialization,
    /// inverted-index and RRF code is fully exercisable + reproducible). When a
    /// real model IS present, dense uses it; sparse still uses the stub until
    /// D3.5 wires the sparse output — is_real_model() reflects only the dense
    /// model state, callers must not assume sparse is model-derived in Phase 1.
    Status EmbedWithSparse(const std::string& text, EmbedWithSparseResult* result,
                           int top_k = 100);

    /// Batch form of EmbedWithSparse (sequential per-item, mirrors EmbedBatch).
    Status EmbedBatchWithSparse(const std::vector<std::string>& texts,
                                std::vector<EmbedWithSparseResult>* results,
                                int top_k = 100);

    int dimension() const { return dim_; }

    /// True if using real ONNX model inference, false if stub mode
    bool is_real_model() const { return !stub_mode_; }

    /// True if CoreML execution provider is active (Apple GPU/Neural Engine)
    /// Valid only after Init() is called. Always false on non-Apple platforms.
    bool is_coreml_active() const { return coreml_active_; }

    /// Set the tokenizer JSON path (defaults to model_path dir + tokenizer.json)
    void set_tokenizer_path(const std::string& path) { tokenizer_path_ = path; }

    /// Set maximum sequence length for tokenization
    void set_max_seq_length(int len) { max_seq_length_ = len; }

    /// Set GPU execution provider ("cpu", "coreml", "auto")
    void set_gpu_provider(const std::string& provider) { gpu_provider_ = provider; }

private:
    std::string model_path_;
    std::string tokenizer_path_;
    std::string gpu_provider_ = "auto";
    int dim_;
    int intra_threads_;
    int inter_threads_;
    int max_seq_length_ = 512;
    bool stub_mode_ = true;
    bool coreml_active_ = false;  // Set in Init() when CoreML EP succeeds

    // Opaque handles (actual types in .cpp via conditional compilation)
    void* env_ = nullptr;       // Ort::Env*
    void* session_ = nullptr;   // Ort::Session*
    void* tokenizer_ = nullptr; // HfTokenizer*

    /// Stub embedding: deterministic fake vectors (fallback when model unavailable)
    Status StubEmbed(const std::string& text, EmbeddingResult* result) const;

    /// Deterministic stub sparse lexical_weights for `text` (F40 S1 standalone).
    /// Hashes whitespace-delimited tokens into the BGE-M3 vocab space
    /// (kBgeM3VocabSize), assigns a stable positive weight per term, then keeps
    /// the top-`top_k` by weight. Empty / whitespace-only text → empty map (the
    /// F40 §6.5 "empty_content" path). Mirrors StubEmbed's determinism so tests
    /// are reproducible; replaced by the real model sparse output at D3.5.
    std::map<uint32_t, float> StubSparse(const std::string& text, int top_k) const;
};

/// BGE-M3 token vocabulary size (250K). term_id values returned by the sparse
/// path are < this; the serializer (F40 §4.2) stores term_id as uint16, so the
/// stub keeps ids in a 16-bit window — the real vocab needs the Phase-2 wider
/// term_id encoding tracked in PHASE2_BACKLOG (F40 §4.2 note "18 bits actual").
inline constexpr uint32_t kBgeM3VocabSize = 250000u;

}  // namespace cortrix
