#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::onnx {

/// The 4 F22 ONNX metrics (F22 §13.5), following the F02 `cortrix_reranker_*`
/// naming style: `cortrix_onnx_<subsystem>_<metric>_<unit>`. These are the
/// infrastructure-layer signals an ops Agent queries to answer "why did the
/// ONNX upgrade fail / what runtime version is loaded / did inference regress".
///
/// Standalone (D3): this is a self-contained, dependency-free recorder + an
/// OpenMetrics text renderer. The F24 `/metrics` scrape ENDPOINT does not exist
/// yet (no metrics registry in the frozen tree) — registering this recorder
/// into that endpoint is cross-Feature wiring deferred to D3.5. Until then the
/// recorder is fully usable + testable in-process, and Render() produces the
/// exact text F24 will serve.
class OnnxMetrics {
 public:
    /// Outcome label for cortrix_onnx_startup_validation_total
    /// (result=ok|version_mismatch|opset_incompatible|inference_check_failed).
    enum class StartupResult {
        kOk = 0,
        kVersionMismatch,
        kOpsetIncompatible,
        kInferenceCheckFailed,
    };

    enum class ProviderFallbackReason {
        kEpInitFailed = 0,
        kSessionInitFailed,
    };

    /// Process-wide instance (metrics are global counters/gauges, like a
    /// Prometheus default registry).
    static OnnxMetrics& Instance();

    // --- cortrix_onnx_startup_validation_total (Counter, label: result) ---
    void RecordStartupValidation(StartupResult result);
    uint64_t StartupValidationCount(StartupResult result) const;

    // --- cortrix_onnx_inference_failed_total (Counter, label: retry_attempt=1) ---
    // Per §13.5 the only label value is retry_attempt=1 (V1.0 does exactly one
    // retry); incremented once per run-time inference that fails after its retry.
    void RecordInferenceFailed();
    uint64_t InferenceFailedCount() const;

    // --- cortrix_onnx_inference_duration_seconds (Histogram, no high-card label) ---
    // D35-MET-03: a full cumulative-bucket histogram (le buckets + +Inf + _sum +
    // _count). The sum+count accessors below are kept (the avg is still derivable
    // and the existing TC depends on them); ObserveInferenceDuration now also lands
    // the sample in a bucket. Bounds straddle the ONNX single-call latency band
    // (F02 §3.4: single-pair inference ~10ms CoreML / ~30ms CPU) — see kInfBounds.
    void ObserveInferenceDuration(double seconds);
    uint64_t InferenceDurationCount() const;
    double InferenceDurationSum() const;

    // --- cortrix_onnx_runtime_version_info (Gauge, Info pattern, labels: major/minor/patch) ---
    // Info pattern: value always 1; the version is carried in the labels so an
    // Agent reads the loaded runtime version straight off the metric. Set once
    // at startup from Runtime::GetRuntimeVersion().
    void SetRuntimeVersionInfo(int major, int minor, int patch);

    // --- Embedding execution-provider state ---
    void SetEmbeddingConfiguredEp(const std::string& ep);
    std::string EmbeddingConfiguredEp() const;
    void SetEmbeddingActiveEp(const std::string& ep);
    std::string EmbeddingActiveEp() const;
    void RecordEmbeddingProviderFallback(const std::string& from,
                                         ProviderFallbackReason reason);
    uint64_t EmbeddingProviderFallbackCount(const std::string& from,
                                            ProviderFallbackReason reason) const;
    void RecordEmbeddingProviderInitFailure(const std::string& provider,
                                            ProviderFallbackReason reason);
    uint64_t EmbeddingProviderInitFailureCount(
        const std::string& provider, ProviderFallbackReason reason) const;

    /// Render ONNX Runtime and embedding EP metrics as OpenMetrics text.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/gauges (test-only — production metrics are monotonic).
    void ResetForTest();

 private:
    OnnxMetrics() = default;

    static constexpr int kStartupResultCount = 4;
    // inference_duration_seconds histogram bucket count (upper bounds in the .cpp
    // kInfBounds[]; the +1 slot is the implicit +Inf bucket).
    static constexpr int kInfBucketCount = 8;
    static constexpr int kProviderFallbackCount = 4;
    static constexpr int kProviderInitFailureCount = 6;

    std::array<std::atomic<uint64_t>, kStartupResultCount> startup_validation_{};
    std::atomic<uint64_t> inference_failed_{0};
    std::atomic<uint64_t> inference_duration_count_{0};
    // double has no std::atomic arithmetic pre-C++20 in a portable way; we store
    // the bit pattern and CAS it (sum is low-contention — one writer per infer).
    std::atomic<uint64_t> inference_duration_sum_bits_{0};
    // Per-bucket counters (NOT cumulative; RenderOpenMetrics accumulates le-wise).
    // Index kInfBucketCount = the +Inf overflow bucket.
    std::array<std::atomic<uint64_t>, kInfBucketCount + 1> inference_duration_bkt_{};
    std::atomic<int> rt_major_{0};
    std::atomic<int> rt_minor_{0};
    std::atomic<int> rt_patch_{0};
    std::atomic<bool> rt_version_set_{false};
    // EP codes: 0=auto, 1=cpu, 2=coreml, 3=cuda, 4=stub.
    std::atomic<int> embedding_configured_ep_{0};
    std::atomic<int> embedding_active_ep_{4};
    // coreml/cuda x ep_init_failed/session_init_failed.
    std::array<std::atomic<uint64_t>, kProviderFallbackCount>
        embedding_provider_fallback_{};
    // cpu/coreml/cuda x ep_init_failed/session_init_failed.
    std::array<std::atomic<uint64_t>, kProviderInitFailureCount>
        embedding_provider_init_failure_{};
};

/// String form of a StartupResult label value (e.g. kVersionMismatch ->
/// "version_mismatch").
const char* ToString(OnnxMetrics::StartupResult result);
const char* ToString(OnnxMetrics::ProviderFallbackReason reason);

}  // namespace cortrix::onnx
