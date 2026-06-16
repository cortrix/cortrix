#include <cstdint>
#include "cortrix/onnx/onnx_metrics.h"

#include <cstring>
#include <sstream>

namespace cortrix::onnx {

const char* ToString(OnnxMetrics::StartupResult result) {
    switch (result) {
        case OnnxMetrics::StartupResult::kOk:                  return "ok";
        case OnnxMetrics::StartupResult::kVersionMismatch:     return "version_mismatch";
        case OnnxMetrics::StartupResult::kOpsetIncompatible:   return "opset_incompatible";
        case OnnxMetrics::StartupResult::kInferenceCheckFailed:return "inference_check_failed";
    }
    return "ok";  // unreachable for a valid enum
}

namespace {

// cortrix_onnx_inference_duration_seconds bucket upper bounds (seconds), aligned
// with OnnxMetrics::kInfBucketCount. No explicit per-call SLA is fixed in F22 /
// OBS_SPEC for ONNX inference, so these straddle the observed single-call band
// (F02 §3.4: bge-reranker single-pair ~10ms CoreML / ~30ms CPU; OnnxEmbedder is
// the same order) with headroom up to 1s for cold/degraded runs.
constexpr double kInfBounds[8] = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0};
constexpr const char* kInfBoundStr[8] = {
    "0.005", "0.01", "0.025", "0.05", "0.1", "0.25", "0.5", "1"};

// Pack/unpack a double through a uint64_t for the atomic sum accumulator.
double BitsToDouble(uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}
uint64_t DoubleToBits(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

}  // namespace

OnnxMetrics& OnnxMetrics::Instance() {
    static OnnxMetrics instance;
    return instance;
}

void OnnxMetrics::RecordStartupValidation(StartupResult result) {
    startup_validation_[static_cast<int>(result)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t OnnxMetrics::StartupValidationCount(StartupResult result) const {
    return startup_validation_[static_cast<int>(result)].load(std::memory_order_relaxed);
}

void OnnxMetrics::RecordInferenceFailed() {
    inference_failed_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t OnnxMetrics::InferenceFailedCount() const {
    return inference_failed_.load(std::memory_order_relaxed);
}

void OnnxMetrics::ObserveInferenceDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    inference_duration_count_.fetch_add(1, std::memory_order_relaxed);
    // CAS-accumulate the sum (low contention: one observe per inference call).
    uint64_t cur = inference_duration_sum_bits_.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        next = DoubleToBits(BitsToDouble(cur) + seconds);
    } while (!inference_duration_sum_bits_.compare_exchange_weak(
        cur, next, std::memory_order_relaxed));
    // Land the sample in the smallest bucket whose le bound it fits (else +Inf).
    int idx = kInfBucketCount;
    for (int j = 0; j < kInfBucketCount; ++j) {
        if (seconds <= kInfBounds[j]) { idx = j; break; }
    }
    inference_duration_bkt_[idx].fetch_add(1, std::memory_order_relaxed);
}

uint64_t OnnxMetrics::InferenceDurationCount() const {
    return inference_duration_count_.load(std::memory_order_relaxed);
}

double OnnxMetrics::InferenceDurationSum() const {
    return BitsToDouble(inference_duration_sum_bits_.load(std::memory_order_relaxed));
}

void OnnxMetrics::SetRuntimeVersionInfo(int major, int minor, int patch) {
    rt_major_.store(major, std::memory_order_relaxed);
    rt_minor_.store(minor, std::memory_order_relaxed);
    rt_patch_.store(patch, std::memory_order_relaxed);
    rt_version_set_.store(true, std::memory_order_relaxed);
}

std::string OnnxMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_onnx_startup_validation_total — one line per result label.
    os << "# HELP cortrix_onnx_startup_validation_total ONNX startup validation outcomes.\n";
    os << "# TYPE cortrix_onnx_startup_validation_total counter\n";
    for (int i = 0; i < kStartupResultCount; ++i) {
        os << "cortrix_onnx_startup_validation_total{result=\""
           << ToString(static_cast<StartupResult>(i)) << "\"} "
           << startup_validation_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_onnx_inference_failed_total — single label retry_attempt="1".
    os << "# HELP cortrix_onnx_inference_failed_total Run-time inference failures after retry.\n";
    os << "# TYPE cortrix_onnx_inference_failed_total counter\n";
    os << "cortrix_onnx_inference_failed_total{retry_attempt=\"1\"} "
       << inference_failed_.load(std::memory_order_relaxed) << "\n";

    // cortrix_onnx_inference_duration_seconds — full histogram (cumulative le
    // buckets + +Inf + _sum + _count). D35-MET-03: was sum+count only.
    os << "# HELP cortrix_onnx_inference_duration_seconds ONNX inference latency.\n";
    os << "# TYPE cortrix_onnx_inference_duration_seconds histogram\n";
    uint64_t inf_cum = 0;
    for (int i = 0; i < kInfBucketCount; ++i) {
        inf_cum += inference_duration_bkt_[i].load(std::memory_order_relaxed);
        os << "cortrix_onnx_inference_duration_seconds_bucket{le=\""
           << kInfBoundStr[i] << "\"} " << inf_cum << "\n";
    }
    inf_cum += inference_duration_bkt_[kInfBucketCount].load(std::memory_order_relaxed);
    os << "cortrix_onnx_inference_duration_seconds_bucket{le=\"+Inf\"} " << inf_cum << "\n";
    os << "cortrix_onnx_inference_duration_seconds_sum "
       << InferenceDurationSum() << "\n";
    os << "cortrix_onnx_inference_duration_seconds_count "
       << inference_duration_count_.load(std::memory_order_relaxed) << "\n";

    // cortrix_onnx_runtime_version_info — Info gauge, value always 1, version in
    // labels. Emitted only once the version has been set (at startup).
    os << "# HELP cortrix_onnx_runtime_version_info Loaded ONNX Runtime version (Info pattern).\n";
    os << "# TYPE cortrix_onnx_runtime_version_info gauge\n";
    if (rt_version_set_.load(std::memory_order_relaxed)) {
        os << "cortrix_onnx_runtime_version_info{major=\""
           << rt_major_.load(std::memory_order_relaxed) << "\",minor=\""
           << rt_minor_.load(std::memory_order_relaxed) << "\",patch=\""
           << rt_patch_.load(std::memory_order_relaxed) << "\"} 1\n";
    }

    return os.str();
}

void OnnxMetrics::ResetForTest() {
    for (auto& c : startup_validation_) c.store(0, std::memory_order_relaxed);
    inference_failed_.store(0, std::memory_order_relaxed);
    inference_duration_count_.store(0, std::memory_order_relaxed);
    inference_duration_sum_bits_.store(0, std::memory_order_relaxed);
    for (auto& b : inference_duration_bkt_) b.store(0, std::memory_order_relaxed);
    rt_major_.store(0, std::memory_order_relaxed);
    rt_minor_.store(0, std::memory_order_relaxed);
    rt_patch_.store(0, std::memory_order_relaxed);
    rt_version_set_.store(false, std::memory_order_relaxed);
}

}  // namespace cortrix::onnx
