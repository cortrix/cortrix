#include "cortrix/query/onnx_complexity_backend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "cortrix/query/router_error.h"

#ifdef CORTRIX_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace fs = std::filesystem;

namespace cortrix::query {

namespace {
// Label order of the model's probs[3] output (model README): 0=chat, 1=simple,
// 2=complex. Index into this with argmax(probs) to get the backend label.
constexpr std::array<const char*, 3> kLabels = {"chat", "simple", "complex"};
}  // namespace

OnnxComplexityBackend::OnnxComplexityBackend(std::string model_path,
                                             std::string vocab_path,
                                             int max_seq_length,
                                             int intra_threads)
    : model_path_(std::move(model_path)),
      vocab_path_(std::move(vocab_path)),
      max_seq_length_(max_seq_length),
      intra_threads_(intra_threads) {}

OnnxComplexityBackend::~OnnxComplexityBackend() {
#ifdef CORTRIX_HAS_ONNXRUNTIME
    delete static_cast<Ort::Session*>(session_);
    delete static_cast<Ort::Env*>(env_);
    session_ = nullptr;
    env_ = nullptr;
#endif
}

Status OnnxComplexityBackend::Init() {
    if (ready_) return Status::Ok();  // idempotent

    // Resolve the vocab path (default = vocab.txt next to model.onnx).
    std::string vocab_path = vocab_path_;
    if (vocab_path.empty() && !model_path_.empty()) {
        vocab_path = (fs::path(model_path_).parent_path() / "vocab.txt").string();
    }

#ifdef CORTRIX_HAS_ONNXRUNTIME
    if (model_path_.empty() || !fs::exists(model_path_)) {
        spdlog::info("OnnxComplexityBackend: model not found at '{}', backend unavailable "
                     "(query routing L2 default-Complex path)", model_path_);
        return Status::Ok();  // unavailable, not an error (offline-friendly)
    }

    // Load the WordPiece vocab first — a model with no usable tokenizer is a hard
    // load failure (returned), distinct from the "no model file" offline case.
    Status tok = tokenizer_.LoadVocab(vocab_path);
    if (!tok.ok()) {
        spdlog::warn("OnnxComplexityBackend: vocab load failed ({}): {}",
                     vocab_path, tok.message());
        return tok;
    }

    try {
        auto* env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "cortrix_complexity");

        Ort::SessionOptions opts;
        opts.SetInterOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        int threads = intra_threads_;
        if (threads <= 0) {
            threads = static_cast<int>(std::min(4u, std::thread::hardware_concurrency()));
        }
        opts.SetIntraOpNumThreads(threads);

        auto* session = new Ort::Session(*env, model_path_.c_str(), opts);

        // Contract check: 2 inputs (input_ids, attention_mask), 1 output (probs).
        if (session->GetInputCount() < 2 || session->GetOutputCount() < 1) {
            spdlog::warn("OnnxComplexityBackend: model has {} inputs / {} outputs "
                         "(expected >=2 / >=1), backend unavailable",
                         session->GetInputCount(), session->GetOutputCount());
            delete session;
            delete env;
            return Status::Internal("OnnxComplexityBackend: unexpected model signature");
        }

        env_ = env;
        session_ = session;
        ready_ = true;
        spdlog::info("OnnxComplexityBackend ready: model={}, vocab={}, threads={}",
                     model_path_, vocab_path, threads);
        return Status::Ok();
    } catch (const Ort::Exception& e) {
        spdlog::warn("OnnxComplexityBackend: ONNX session init failed: {}", e.what());
        return Status::Internal(std::string("OnnxComplexityBackend init: ") + e.what());
    } catch (const std::exception& e) {
        spdlog::warn("OnnxComplexityBackend: unexpected init error: {}", e.what());
        return Status::Internal(std::string("OnnxComplexityBackend init: ") + e.what());
    }
#else
    (void)vocab_path;
    spdlog::info("OnnxComplexityBackend: built without ONNX Runtime, backend unavailable");
    return Status::Ok();
#endif
}

ComplexityBackendResult OnnxComplexityBackend::Infer(const std::string& query) {
    if (!ready_) {
        // The classifier checks IsAvailable() before calling Infer(); reaching
        // here means a misuse — surface it on the transient (retry/degrade) path.
        throw RouterInferenceError("OnnxComplexityBackend::Infer called while unavailable");
    }

#ifdef CORTRIX_HAS_ONNXRUNTIME
    auto* session = static_cast<Ort::Session*>(session_);

    WordPieceTokenizer::Encoded enc;
    Status tok = tokenizer_.Encode(query, max_seq_length_, &enc);
    if (!tok.ok()) {
        // Tokenization should never fail for in-range input; treat as transient.
        throw RouterInferenceError("OnnxComplexityBackend: tokenization failed: " +
                                   tok.message());
    }

    auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const int64_t seq_len = static_cast<int64_t>(enc.input_ids.size());
    std::array<int64_t, 2> shape = {1, seq_len};

    auto ids_tensor = Ort::Value::CreateTensor<int64_t>(
        mem_info, enc.input_ids.data(), enc.input_ids.size(), shape.data(), shape.size());
    auto mask_tensor = Ort::Value::CreateTensor<int64_t>(
        mem_info, enc.attention_mask.data(), enc.attention_mask.size(),
        shape.data(), shape.size());

    Ort::AllocatorWithDefaultOptions alloc;
    auto in0 = session->GetInputNameAllocated(0, alloc);
    auto in1 = session->GetInputNameAllocated(1, alloc);
    auto out0 = session->GetOutputNameAllocated(0, alloc);
    const char* input_names[] = {in0.get(), in1.get()};
    const char* output_names[] = {out0.get()};

    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(ids_tensor));
    inputs.push_back(std::move(mask_tensor));

    // One retry on a transient ONNX fault (mirrors OnnxEmbedder), then surface
    // RouterInferenceError → QueryComplexityClassifier's L3 retry/degrade path.
    std::vector<Ort::Value> outputs;
    bool ok = false;
    std::string err_what;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        try {
            outputs = session->Run(Ort::RunOptions{nullptr}, input_names,
                                   inputs.data(), inputs.size(), output_names, 1);
            ok = true;
        } catch (const Ort::Exception& e) {
            err_what = e.what();
            spdlog::warn("OnnxComplexityBackend inference failed (attempt {}/2): {}",
                         attempt + 1, e.what());
        }
    }
    if (!ok) {
        throw RouterInferenceError("OnnxComplexityBackend: ONNX Run failed after 1 retry: " +
                                   err_what);
    }

    // Output probs [1, 3] (softmax in-graph). argmax → label, max → confidence.
    auto& out = outputs[0];
    auto out_shape = out.GetTensorTypeAndShapeInfo().GetShape();
    const float* probs = out.GetTensorData<float>();
    int64_t n_classes = (out_shape.size() >= 2) ? out_shape[out_shape.size() - 1] : 3;
    if (n_classes < 1) {
        throw RouterInferenceError("OnnxComplexityBackend: empty probs output");
    }
    if (n_classes > 3) n_classes = 3;  // guard: only the 3 known labels are mapped

    int best = 0;
    float best_p = probs[0];
    for (int64_t c = 1; c < n_classes; ++c) {
        if (probs[c] > best_p) {
            best_p = probs[c];
            best = static_cast<int>(c);
        }
    }

    ComplexityBackendResult r;
    r.label = kLabels[best];
    r.confidence = best_p;
    return r;
#else
    (void)query;
    throw RouterInferenceError("OnnxComplexityBackend: built without ONNX Runtime");
#endif
}

}  // namespace cortrix::query
