#pragma once
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/reranker.h"
#include "cortrix/reranker/circuit_breaker.h"
#include "cortrix/reranker/reranker_config.h"
#include "cortrix/reranker/reranker_metrics.h"
#include "cortrix/reranker/reranker_thread_pool.h"
#include "cortrix/spc/hf_tokenizer.h"
#include "cortrix/store/chunk_store.h"

namespace cortrix::reranker {

/// Result of one ScoreBatch per-passage inference task, returned BY VALUE from the
/// pool task (R02-C2 use-after-free / data-race fix). The previous design captured
/// the per-iteration `task_failed` / `reason` stack locals by reference; a task that
/// timed out kept running on its worker and wrote those fields after the stack slot
/// had been destroyed / reused by the next loop iteration (UAF + concurrent write
/// across the 4 workers). Returning the outcome by value confines it to the task's
/// own future, which the caller discards on timeout — the worker never touches the
/// caller's frame.
struct RerankTaskResult {
    float score = 0.0f;                          ///< inference score (0 on failure)
    bool failed = false;                         ///< true on ONNX-exception / OOM
    RerankerMetrics::FailedTaskReason reason{};  ///< failure label (valid iff failed)
};

/// OnnxReranker — V1 sole IReranker implementation (bge-reranker-v2-m3 ONNX
/// Cross-Encoder, F02 §2.2).
///
/// Sharing (F02 §2.4-bis): borrows the process-wide Ort::Env (OrtEnvSingleton)
/// and the bge-m3 HfTokenizer (TokenizerRegistry); owns an independent
/// Ort::Session. Ort types are kept out of this header (opaque void* session_),
/// mirroring the in-tree OnnxEmbedder pattern so non-ONNX TUs can include it.
///
/// Standalone (D3): with no model file present Init() runs in stub mode (no
/// Session, Score returns a deterministic stub) so the class is constructible and
/// testable offline; the real-model + fail-fast paths land in S1.3.
class OnnxReranker : public IReranker {
public:
    /// @param config       reranker config (model/tokenizer paths, EP, limits)
    /// @param chunk_store   borrowed; used by Rerank() to reverse-look-up chunk
    ///                      text. May be null for Score/ScoreBatch-only use.
    OnnxReranker(const RerankerConfig& config, store::ChunkStore* chunk_store);
    ~OnnxReranker() override;

    OnnxReranker(const OnnxReranker&) = delete;
    OnnxReranker& operator=(const OnnxReranker&) = delete;

    /// Load tokenizer (shared registry) + create the ONNX Session. Returns the
    /// fail-fast error (S1.3) on failure. In stub mode (no model file) returns Ok.
    Status Init();

    /// Stop the worker pool and join in-flight scoring tasks. Idempotent; the
    /// dtor calls it. A timed-out ScoreBatch task keeps RUNNING on its pool
    /// worker after ScoreBatch returns (§3.3) — destroying the reranker while
    /// it runs is a use-after-free on session_ (and a vptr race for derived
    /// classes). Subclasses overriding ScoreForTask MUST call Shutdown() in
    /// their own dtor so workers quiesce before the vtable is demoted.
    void Shutdown();

    // --- IReranker ---
    float Score(const char* query, const char* passage) override;
    std::vector<float> ScoreBatch(const char* query,
                                  const std::vector<const char*>& passages) override;
    std::vector<retrieval::RankedChunk> Rerank(
        const std::vector<retrieval::ScoredResult>& candidates,
        const std::string& query) override;
    const char* Name() const override { return "OnnxReranker"; }

    // --- Introspection (test / health) ---
    bool is_real_model() const { return !stub_mode_; }
    bool is_coreml_active() const { return active_ep_ == "coreml"; }
    bool is_cuda_active() const { return active_ep_ == "cuda"; }
    /// /health contract (S1.3): false until Init() succeeds (endpoint 503), true
    /// after (endpoint 200). A failed Init() (fail-fast) leaves this false.
    bool is_ready() const { return ready_; }
    const char* configured_ep() const { return config_.execution_provider.c_str(); }
    const char* active_ep() const { return active_ep_.c_str(); }
    const RerankerConfig& config() const { return config_; }

    // --- Health / introspection for the circuit breaker + pool (S3.4) ---
    CircuitBreakerState circuit_state() const {
        return circuit_breaker_ ? circuit_breaker_->State() : CircuitBreakerState::kClosed;
    }
    bool has_thread_pool() const { return thread_pool_ != nullptr; }

protected:
    /// Single-pair inference invoked by ScoreBatch's per-task body. The real ONNX
    /// path (later Wave) runs Score() here and MAY throw (Ort::Exception / OOM);
    /// ScoreBatch guards it (try/catch → score=0 + failed_tasks_total + breaker).
    /// virtual so tests can inject ONNX-exception / OOM failures (S3.4 §7 DoD
    /// "ONNX session exception injection"). Default delegates to the public Score().
    virtual float ScoreForTask(const char* query, const char* passage) {
        return Score(query, passage);
    }

private:
    /// Stub scorer used when no real model is loaded (deterministic, offline).
    float StubScore(const char* query, const char* passage) const;

    RerankerConfig config_;
    store::ChunkStore* chunk_store_;             // borrowed, not owned
    std::shared_ptr<HfTokenizer> tokenizer_;     // shared via TokenizerRegistry
    std::unique_ptr<RerankerThreadPool<RerankTaskResult>> thread_pool_;  // S2.1 (built in Init)
    std::unique_ptr<CircuitBreaker> circuit_breaker_;     // S2.4 (built in Init)
    bool stub_mode_ = true;
    std::string active_ep_ = "stub";
    bool ready_ = false;

    // Opaque ONNX handles (concrete types only in the .cpp via conditional
    // compilation). env_ is borrowed from OrtEnvSingleton (not deleted);
    // session_ is owned (deleted in dtor).
    void* env_ = nullptr;       // Ort::Env*  (borrowed)
    void* session_ = nullptr;   // Ort::Session* (owned)
};

}  // namespace cortrix::reranker
