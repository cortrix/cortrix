#include <cstdint>
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/hf_tokenizer.h"
#include "cortrix/onnx/onnx_error.h"
#include "cortrix/onnx/onnx_metrics.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>

#ifdef CORTRIX_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef CORTRIX_HAS_COREML
#include <coreml_provider_factory.h>
#endif
#endif

namespace fs = std::filesystem;

namespace cortrix {

OnnxEmbedder::OnnxEmbedder(const std::string& model_path, int dim,
                           int intra_threads, int inter_threads)
    : model_path_(model_path), dim_(dim),
      intra_threads_(intra_threads), inter_threads_(inter_threads) {}

OnnxEmbedder::~OnnxEmbedder() {
#ifdef CORTRIX_HAS_ONNXRUNTIME
    if (!stub_mode_) {
        delete static_cast<Ort::Session*>(session_);
        delete static_cast<Ort::Env*>(env_);
        delete static_cast<HfTokenizer*>(tokenizer_);
        session_ = nullptr;
        env_ = nullptr;
        tokenizer_ = nullptr;
    }
#endif
}

Status OnnxEmbedder::Init() {
#ifdef CORTRIX_HAS_ONNXRUNTIME
    // Try real ONNX Runtime initialization if model file exists
    if (!model_path_.empty() && fs::exists(model_path_)) {
        try {
            // Determine tokenizer path
            std::string tok_path = tokenizer_path_;
            if (tok_path.empty()) {
                tok_path = (fs::path(model_path_).parent_path() / "tokenizer.json").string();
            }

            // Load tokenizer
            auto* tokenizer = new HfTokenizer();
            auto tok_status = tokenizer->Load(tok_path);
            if (!tok_status.ok()) {
                spdlog::warn("Failed to load tokenizer from {}: {}, falling back to stub embeddings",
                             tok_path, tok_status.message());
                delete tokenizer;
                // Fall through to stub mode
            } else {
                // Create ONNX Runtime environment
                auto* env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "cortrix_embedder");

                Ort::SessionOptions opts;
                opts.SetInterOpNumThreads(inter_threads_);
                opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

                // Try CoreML first, before setting intra_threads, so we can auto-tune
#ifdef CORTRIX_HAS_COREML
                if (gpu_provider_ == "coreml" || gpu_provider_ == "auto") {
                    uint32_t coreml_flags = COREML_FLAG_ENABLE_ON_SUBGRAPH;
                    auto coreml_status = OrtSessionOptionsAppendExecutionProvider_CoreML(opts, coreml_flags);
                    if (coreml_status != nullptr) {
                        spdlog::warn("CoreML EP initialization failed, falling back to CPU");
                    } else {
                        spdlog::info("CoreML execution provider enabled");
                        coreml_active_ = true;
                    }
                }
#endif

                // Auto-detect intra_threads when set to 0:
                //   CoreML active → 2 (GPU handles heavy ops; fewer CPU threads reduce contention)
                //   CPU only      → min(4, hardware_concurrency) (saturate available cores)
                int effective_threads = intra_threads_;
                if (effective_threads == 0) {
                    effective_threads = coreml_active_
                        ? 2
                        : static_cast<int>(std::min(4u, std::thread::hardware_concurrency()));
                    spdlog::info("onnx_intra_threads auto={} (coreml_active={})",
                                 effective_threads, coreml_active_);
                }
                opts.SetIntraOpNumThreads(effective_threads);

                // Create session
                auto* session = new Ort::Session(*env, model_path_.c_str(), opts);

                // Verify model has expected inputs
                size_t num_inputs = session->GetInputCount();
                if (num_inputs < 2) {
                    spdlog::warn("ONNX model has {} inputs (expected >=2), falling back to stub",
                                 num_inputs);
                    delete session;
                    delete env;
                    delete tokenizer;
                } else {
                    // Success: store handles
                    env_ = env;
                    session_ = session;
                    tokenizer_ = tokenizer;
                    stub_mode_ = false;

                    spdlog::info("ONNX embedder initialized: model={}, dim={}", model_path_, dim_);
                    return Status::Ok();
                }
            }
        } catch (const Ort::Exception& e) {
            spdlog::warn("ONNX Runtime initialization failed: {}, falling back to stub embeddings",
                         e.what());
        } catch (const std::exception& e) {
            spdlog::warn("Unexpected error loading ONNX model: {}, falling back to stub embeddings",
                         e.what());
        }
    } else if (!model_path_.empty()) {
        spdlog::info("ONNX model not found at {}, using stub embeddings", model_path_);
    }
#endif

    // Stub mode: no real model, use deterministic fake embeddings
    stub_mode_ = true;
    session_ = reinterpret_cast<void*>(1);  // Non-null sentinel for compatibility
    spdlog::info("ONNX embedder initialized in stub mode (no real model)");
    return Status::Ok();
}

Status OnnxEmbedder::Embed(const std::string& text, EmbeddingResult* result) {
    if (!result) {
        return Status::InvalidArgument("null result pointer");
    }
    if (!session_) {
        return Status::Internal("embedder not initialized, call Init() first");
    }

    auto start = std::chrono::steady_clock::now();

#ifdef CORTRIX_HAS_ONNXRUNTIME
    if (!stub_mode_ && tokenizer_) {
        // --- Real ONNX Runtime inference ---
        auto* tokenizer = static_cast<HfTokenizer*>(tokenizer_);
        auto* session = static_cast<Ort::Session*>(session_);

        // Tokenize input text
        HfTokenizer::Encoded encoded;
        auto tok_status = tokenizer->Encode(text, max_seq_length_, &encoded);
        if (!tok_status.ok()) {
            return tok_status;
        }

        // Prepare ONNX input tensors
        auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        int64_t seq_len = static_cast<int64_t>(encoded.input_ids.size());
        std::array<int64_t, 2> shape = {1, seq_len};

        auto input_ids_tensor = Ort::Value::CreateTensor<int64_t>(
            mem_info, encoded.input_ids.data(), encoded.input_ids.size(),
            shape.data(), shape.size());

        auto attention_mask_tensor = Ort::Value::CreateTensor<int64_t>(
            mem_info, encoded.attention_mask.data(), encoded.attention_mask.size(),
            shape.data(), shape.size());

        // Get input/output names from model
        Ort::AllocatorWithDefaultOptions alloc;
        auto input_name_0 = session->GetInputNameAllocated(0, alloc);
        auto input_name_1 = session->GetInputNameAllocated(1, alloc);
        auto output_name_0 = session->GetOutputNameAllocated(0, alloc);

        const char* input_names[] = {input_name_0.get(), input_name_1.get()};
        const char* output_names[] = {output_name_0.get()};

        // Run inference
        std::vector<Ort::Value> input_tensors;
        input_tensors.push_back(std::move(input_ids_tensor));
        input_tensors.push_back(std::move(attention_mask_tensor));

        // F22 §10 / §8.3 (D35-MET-11): run-time inference is guarded with exactly
        // one retry (transient errors are often momentary — alloc pressure, kernel
        // hiccup). On a 2nd failure surface CX_ERR_ONNX_INFERENCE_FAILED
        // (transient/retryable, retry_after_ms=200) + bump
        // cortrix_onnx_inference_failed_total{retry_attempt="1"}. A successful run
        // feeds cortrix_onnx_inference_duration_seconds.
        std::vector<Ort::Value> output_tensors;
        bool inference_ok = false;
        std::string onnx_err_what;
        for (int attempt = 0; attempt < 2 && !inference_ok; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            try {
                output_tensors = session->Run(
                    Ort::RunOptions{nullptr},
                    input_names, input_tensors.data(), input_tensors.size(),
                    output_names, 1);
                inference_ok = true;
            } catch (const Ort::Exception& e) {
                onnx_err_what = e.what();
                spdlog::warn("ONNX inference failed (attempt {}/2): {}", attempt + 1, e.what());
            }
        }
        if (!inference_ok) {
            // Both the initial run and its single retry failed → transient infra
            // error. last_input_shape = the [1, seq_len] we fed (F22 §8.3
            // structured_data). The Agent-friendly body (op_kernel/last_input_shape)
            // is re-inflated at the API/SDK boundary via MakeOnnxError().
            onnx::OnnxMetrics::Instance().RecordInferenceFailed();
            std::string detail = "ONNX Run() failed after 1 retry";
            if (!onnx_err_what.empty()) detail += ": " + onnx_err_what;
            detail += " (last_input_shape=[1," + std::to_string(seq_len) + "])";
            return onnx::OnnxStatus(onnx::OnnxErrorCode::kInferenceFailed, detail);
        }

        // Extract CLS token embedding (position 0)
        auto& output_tensor = output_tensors[0];
        auto output_shape = output_tensor.GetTensorTypeAndShapeInfo().GetShape();
        const float* output_data = output_tensor.GetTensorData<float>();

        // output_shape should be [1, seq_len, hidden_dim]
        int hidden_dim = (output_shape.size() >= 3) ? static_cast<int>(output_shape[2]) : dim_;
        int actual_dim = std::min(hidden_dim, dim_);

        result->vector.resize(actual_dim);
        for (int i = 0; i < actual_dim; i++) {
            result->vector[i] = output_data[i];  // CLS token = position 0
        }

        // L2 normalize
        float norm = 0.0f;
        for (int i = 0; i < actual_dim; i++) {
            norm += result->vector[i] * result->vector[i];
        }
        norm = std::sqrt(norm);
        if (norm > 0.0f) {
            for (int i = 0; i < actual_dim; i++) {
                result->vector[i] /= norm;
            }
        }

        result->dim = actual_dim;
        auto end = std::chrono::steady_clock::now();
        result->inference_time_ms = std::chrono::duration<float, std::milli>(end - start).count();
        // D35-MET-11: feed cortrix_onnx_inference_duration_seconds (real-model path
        // only; the stub path below is not an ONNX inference).
        onnx::OnnxMetrics::Instance().ObserveInferenceDuration(
            std::chrono::duration<double>(end - start).count());
        return Status::Ok();
    }
#endif

    // Stub fallback
    return StubEmbed(text, result);
}

Status OnnxEmbedder::EmbedBatch(const std::vector<std::string>& texts,
                                std::vector<EmbeddingResult>* results) {
    if (!results) {
        return Status::InvalidArgument("null results pointer");
    }
    results->clear();
    results->reserve(texts.size());

    // Sequential per-item inference (batch=1 each call).
    // CoreML EP compiles the model for batch=1; passing batch>1 causes an
    // uninterruptible hang. For CPU-only environments true batch inference
    // would help, but keeping it simple for MVP.
    for (const auto& text : texts) {
        EmbeddingResult r;
        Status s = Embed(text, &r);
        if (!s.ok()) return s;
        results->push_back(std::move(r));
    }
    return Status::Ok();
}

// --- F40 S1: dense + sparse single-pass embedding ---

Status OnnxEmbedder::EmbedWithSparse(const std::string& text,
                                     EmbedWithSparseResult* result, int top_k) {
    if (!result) {
        return Status::InvalidArgument("null result pointer");
    }
    auto start = std::chrono::steady_clock::now();

    // Dense half reuses the frozen real/stub embedding path (F22). On failure we
    // propagate the dense error verbatim — per F40 §7.1 L1, a dense+sparse total
    // failure is the chunk-write failure (CX_ERR_F40_INFERENCE_FAILED is raised
    // by the caller around this), so we surface the dense Status unchanged.
    EmbeddingResult dense;
    Status s = Embed(text, &dense);
    if (!s.ok()) {
        return s;
    }
    result->dense = std::move(dense.vector);

    // Sparse half: standalone deterministic stub (D3). The real BGE-M3 sparse
    // head is a 2nd model output that requires a re-exported model → D3.5; here
    // the stub exercises the full serialize / inverted-index / RRF chain.
    result->sparse = StubSparse(text, top_k);

    auto end = std::chrono::steady_clock::now();
    result->inference_time_ms =
        std::chrono::duration<float, std::milli>(end - start).count();
    return Status::Ok();
}

Status OnnxEmbedder::EmbedBatchWithSparse(
    const std::vector<std::string>& texts,
    std::vector<EmbedWithSparseResult>* results, int top_k) {
    if (!results) {
        return Status::InvalidArgument("null results pointer");
    }
    results->clear();
    results->reserve(texts.size());
    // Sequential per-item (mirrors EmbedBatch — CoreML EP is batch=1 locked).
    for (const auto& text : texts) {
        EmbedWithSparseResult r;
        Status s = EmbedWithSparse(text, &r, top_k);
        if (!s.ok()) return s;
        results->push_back(std::move(r));
    }
    return Status::Ok();
}

std::map<uint32_t, float> OnnxEmbedder::StubSparse(const std::string& text,
                                                   int top_k) const {
    // Whitespace-tokenize; each distinct token hashes to a vocab id (kept inside
    // the uint16 window so the F40 §4.2 uint16 serializer round-trips — the real
    // 18-bit vocab encoding is a Phase-2 widening, see header note). Weight is a
    // stable [0,1) value per token; duplicate tokens accumulate weight (SPLADE
    // term-frequency intuition). Empty / whitespace-only → empty map (§6.5
    // empty_content path).
    std::map<uint32_t, float> sparse;
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        uint32_t h = 2166136261u;  // FNV-1a basis
        for (char c : token) {
            h ^= static_cast<uint8_t>(c);
            h *= 16777619u;
        }
        // Map into [1, 65535] (id 0 reserved as "no term") — uint16 window.
        uint32_t term_id = (h % 65535u) + 1u;
        // Stable weight in (0,1): derive from a different mix of the hash.
        float w = static_cast<float>((h >> 8) & 0xFFFFu) / 65535.0f;
        if (w <= 0.0f) w = 1e-4f;  // keep strictly positive (active term)
        sparse[term_id] += w;
        token.clear();
    };
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            flush();
        } else {
            token.push_back(c);
        }
    }
    flush();

    // top-K truncation by weight DESC (F40-6). top_k<=0 → keep all.
    if (top_k > 0 && static_cast<int>(sparse.size()) > top_k) {
        std::vector<std::pair<uint32_t, float>> items(sparse.begin(), sparse.end());
        std::partial_sort(
            items.begin(), items.begin() + top_k, items.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        std::map<uint32_t, float> truncated;
        for (int i = 0; i < top_k; ++i) {
            truncated.insert(items[i]);
        }
        return truncated;
    }
    return sparse;
}

// --- Stub implementation (deterministic fake vectors) ---

Status OnnxEmbedder::StubEmbed(const std::string& text, EmbeddingResult* result) const {
    auto start = std::chrono::steady_clock::now();

    result->vector.resize(dim_);
    uint32_t hash = 0;
    for (char c : text) {
        hash = hash * 31 + static_cast<uint8_t>(c);
    }

    for (int i = 0; i < dim_; ++i) {
        uint32_t val = hash ^ (static_cast<uint32_t>(i) * 2654435761u);
        result->vector[i] = (static_cast<float>(val & 0xFFFF) / 32768.0f) - 1.0f;
    }

    // Normalize to unit length
    float norm = 0.0f;
    for (int i = 0; i < dim_; ++i) {
        norm += result->vector[i] * result->vector[i];
    }
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (int i = 0; i < dim_; ++i) {
            result->vector[i] /= norm;
        }
    }

    result->dim = dim_;
    auto end = std::chrono::steady_clock::now();
    result->inference_time_ms = std::chrono::duration<float, std::milli>(end - start).count();

    return Status::Ok();
}

}  // namespace cortrix
