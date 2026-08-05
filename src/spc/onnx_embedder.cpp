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
#include <memory>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>
#include "cortrix/ml/execution_provider.h"

#ifdef CORTRIX_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef CORTRIX_HAS_COREML
#include <coreml_provider_factory.h>
#endif
#endif

namespace fs = std::filesystem;

namespace cortrix {

namespace {

#ifdef CORTRIX_HAS_ONNXRUNTIME
Status AppendExecutionProvider(Ort::SessionOptions* options,
                               ml::ExecutionProvider provider) {
    OrtStatus* status = nullptr;
    switch (provider) {
        case ml::ExecutionProvider::kCpu:
            return Status::Ok();
        case ml::ExecutionProvider::kCoreMl:
#ifdef CORTRIX_HAS_COREML
            status = OrtSessionOptionsAppendExecutionProvider_CoreML(
                *options, COREML_FLAG_ENABLE_ON_SUBGRAPH);
            break;
#else
            return Status::Unavailable("CoreML execution provider is not compiled in");
#endif
        case ml::ExecutionProvider::kCuda:
#ifdef CORTRIX_HAS_CUDA
            {
                OrtCUDAProviderOptions cuda_options{};
                cuda_options.device_id = 0;
                status = Ort::GetApi().SessionOptionsAppendExecutionProvider_CUDA(
                    *options, &cuda_options);
            }
            break;
#else
            return Status::Unavailable("CUDA execution provider is not compiled in");
#endif
        case ml::ExecutionProvider::kAuto:
            return Status::InvalidArgument("auto must be resolved before session creation");
    }

    if (status == nullptr) return Status::Ok();
    const char* message = Ort::GetApi().GetErrorMessage(status);
    const std::string detail = message == nullptr ? "unknown ONNX Runtime error" : message;
    Ort::GetApi().ReleaseStatus(status);
    return Status::Unavailable(std::string(ml::ExecutionProviderName(provider)) +
                               " execution provider initialization failed: " + detail);
}
#endif

}  // namespace

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
    ml::ExecutionProvider configured_provider;
    Status provider_status =
        ml::ParseExecutionProvider(execution_provider_, &configured_provider);
    if (!provider_status.ok()) return provider_status;
    provider_status = ml::ValidateExecutionProviderForBuild(configured_provider);
    if (!provider_status.ok()) return provider_status;

    execution_provider_ = ml::ExecutionProviderName(configured_provider);
    active_ep_ = "stub";
    onnx::OnnxMetrics::Instance().SetEmbeddingConfiguredEp(execution_provider_);
    onnx::OnnxMetrics::Instance().SetEmbeddingActiveEp(active_ep_);

    if (model_path_.empty()) {
        stub_mode_ = true;
        session_ = reinterpret_cast<void*>(1);
        spdlog::info("ONNX embedder initialized in stub mode (no model configured)");
        return Status::Ok();
    }

#ifdef CORTRIX_HAS_ONNXRUNTIME
    if (!fs::exists(model_path_)) {
        return Status::NotFound("ONNX embedding model not found: " + model_path_);
    }

    std::string tok_path = tokenizer_path_;
    if (tok_path.empty()) {
        tok_path = (fs::path(model_path_).parent_path() / "tokenizer.json").string();
    }
    auto tokenizer = std::make_unique<HfTokenizer>();
    Status tok_status = tokenizer->Load(tok_path);
    if (!tok_status.ok()) {
        return Status::Unavailable("failed to load embedding tokenizer from " + tok_path +
                                   ": " + tok_status.message());
    }

    try {
        auto env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                              "cortrix_embedder");
        auto create_session = [&](ml::ExecutionProvider provider,
                                  std::unique_ptr<Ort::Session>* session,
                                  onnx::OnnxMetrics::ProviderFallbackReason* reason) {
            Ort::SessionOptions options;
            options.SetInterOpNumThreads(inter_threads_);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            Status append_status = AppendExecutionProvider(&options, provider);
            if (!append_status.ok()) {
                *reason = onnx::OnnxMetrics::ProviderFallbackReason::kEpInitFailed;
                return append_status;
            }

            int effective_threads = intra_threads_;
            if (effective_threads == 0) {
                const unsigned hardware_threads =
                    std::max(1u, std::thread::hardware_concurrency());
                effective_threads = provider == ml::ExecutionProvider::kCpu
                    ? static_cast<int>(std::min(4u, hardware_threads))
                    : 2;
            }
            options.SetIntraOpNumThreads(effective_threads);

            try {
                *session = std::make_unique<Ort::Session>(
                    *env, model_path_.c_str(), options);
                return Status::Ok();
            } catch (const Ort::Exception& e) {
                *reason = onnx::OnnxMetrics::ProviderFallbackReason::kSessionInitFailed;
                return Status::Unavailable(std::string("ONNX embedding session creation failed: ") +
                                           e.what());
            }
        };

        const bool allow_cpu_fallback =
            configured_provider == ml::ExecutionProvider::kAuto;
        const ml::ExecutionProvider preferred_provider = allow_cpu_fallback
            ? ml::PreferredAutoExecutionProvider()
            : configured_provider;
        ml::ExecutionProvider active_provider = preferred_provider;
        std::unique_ptr<Ort::Session> session;
        onnx::OnnxMetrics::ProviderFallbackReason fallback_reason =
            onnx::OnnxMetrics::ProviderFallbackReason::kSessionInitFailed;
        Status session_status =
            create_session(preferred_provider, &session, &fallback_reason);

        if (!session_status.ok()) {
            onnx::OnnxMetrics::Instance().RecordEmbeddingProviderInitFailure(
                ml::ExecutionProviderName(preferred_provider), fallback_reason);
        }

        if (!session_status.ok() && allow_cpu_fallback &&
            preferred_provider != ml::ExecutionProvider::kCpu) {
            const std::string from = ml::ExecutionProviderName(preferred_provider);
            onnx::OnnxMetrics::Instance().RecordEmbeddingProviderFallback(
                from, fallback_reason);
            spdlog::warn("Embedding {} EP unavailable ({}); auto falling back to CPU",
                         from, session_status.message());
            active_provider = ml::ExecutionProvider::kCpu;
            session_status = create_session(active_provider, &session, &fallback_reason);
            if (!session_status.ok()) {
                onnx::OnnxMetrics::Instance().RecordEmbeddingProviderInitFailure(
                    ml::ExecutionProviderName(active_provider), fallback_reason);
            }
        }
        if (!session_status.ok()) return session_status;

        const size_t num_inputs = session->GetInputCount();
        if (num_inputs < 2) {
            return Status::InvalidArgument(
                "ONNX embedding model must expose at least two inputs; got " +
                std::to_string(num_inputs));
        }

        env_ = env.release();
        session_ = session.release();
        tokenizer_ = tokenizer.release();
        stub_mode_ = false;
        active_ep_ = ml::ExecutionProviderName(active_provider);
        onnx::OnnxMetrics::Instance().SetEmbeddingActiveEp(active_ep_);
        spdlog::info("ONNX embedder initialized: model={}, dim={}, configured_ep={}, active_ep={}",
                     model_path_, dim_, execution_provider_, active_ep_);
        return Status::Ok();
    } catch (const Ort::Exception& e) {
        return Status::Unavailable(std::string("ONNX Runtime initialization failed: ") +
                                   e.what());
    } catch (const std::exception& e) {
        return Status::Unavailable(std::string("embedding initialization failed: ") +
                                   e.what());
    }
#else
    return Status::Unavailable(
        "embedding model configured but Cortrix was built without ONNX Runtime");
#endif
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
        auto tok_status = tokenizer->EncodeNoPad(text, max_seq_length_, &encoded);
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

        // Run-time inference is guarded with exactly
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
            // error. last_input_shape = the [1, seq_len] we fed (the
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

// --- dense + sparse single-pass embedding ---

Status OnnxEmbedder::EmbedWithSparse(const std::string& text,
                                     EmbedWithSparseResult* result, int top_k) {
    if (!result) {
        return Status::InvalidArgument("null result pointer");
    }
    auto start = std::chrono::steady_clock::now();

    // Dense half reuses the frozen real/stub embedding path. On failure we
    // propagate the dense error verbatim — per the L1 rule, a dense+sparse total
    // failure is the chunk-write failure (CX_ERR_SPARSE_INFERENCE_FAILED is raised
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
    // the uint16 window so the uint16 serializer round-trips — the real
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

    // top-K truncation by weight DESC. top_k<=0 → keep all.
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
