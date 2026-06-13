#include "cortrix/retrieval/sparse_metrics.h"

#include <cstring>
#include <sstream>

namespace cortrix::retrieval {

namespace {
// Pack/unpack a double through uint64_t for the atomic gauge (no portable
// atomic<double> arithmetic pre-C++20; gauge is last-write-wins, one writer).
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

const char* ToString(SparseMetrics::InferenceStatus status) {
    switch (status) {
        case SparseMetrics::InferenceStatus::kSuccess:  return "success";
        case SparseMetrics::InferenceStatus::kFailed:   return "failed";
        case SparseMetrics::InferenceStatus::kFallback: return "fallback";
    }
    return "success";
}

const char* ToString(SparseMetrics::ViaPath path) {
    switch (path) {
        case SparseMetrics::ViaPath::kSparse:            return "sparse";
        case SparseMetrics::ViaPath::kFallbackDenseFts5: return "fallback_dense_fts5";
    }
    return "sparse";
}

SparseMetrics& SparseMetrics::Instance() {
    static SparseMetrics instance;
    return instance;
}

void SparseMetrics::RecordInference(InferenceStatus status) {
    inference_[static_cast<int>(status)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t SparseMetrics::InferenceCount(InferenceStatus status) const {
    return inference_[static_cast<int>(status)].load(std::memory_order_relaxed);
}

void SparseMetrics::SetFallbackRatio(double ratio) {
    fallback_ratio_bits_.store(DoubleToBits(ratio), std::memory_order_relaxed);
}

double SparseMetrics::FallbackRatio() const {
    return BitsToDouble(fallback_ratio_bits_.load(std::memory_order_relaxed));
}

void SparseMetrics::SetInvertedIndexSizeBytes(uint64_t bytes) {
    inverted_index_size_bytes_.store(bytes, std::memory_order_relaxed);
}

uint64_t SparseMetrics::InvertedIndexSizeBytes() const {
    return inverted_index_size_bytes_.load(std::memory_order_relaxed);
}

void SparseMetrics::RecordQueryViaPath(ViaPath path) {
    via_path_[static_cast<int>(path)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t SparseMetrics::QueryViaPathCount(ViaPath path) const {
    return via_path_[static_cast<int>(path)].load(std::memory_order_relaxed);
}

std::string SparseMetrics::RenderOpenMetrics() const {
    std::ostringstream os;
    os << "# TYPE cortrix_bge_m3_sparse_inference_total counter\n";
    for (int i = 0; i < kInferenceStatusCount; ++i) {
        os << "cortrix_bge_m3_sparse_inference_total{status=\""
           << ToString(static_cast<InferenceStatus>(i)) << "\"} "
           << inference_[i].load(std::memory_order_relaxed) << "\n";
    }
    os << "# TYPE cortrix_bge_m3_sparse_fallback_ratio gauge\n";
    os << "cortrix_bge_m3_sparse_fallback_ratio " << FallbackRatio() << "\n";

    os << "# TYPE cortrix_bge_m3_sparse_inverted_index_size_bytes gauge\n";
    os << "cortrix_bge_m3_sparse_inverted_index_size_bytes "
       << inverted_index_size_bytes_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_bge_m3_sparse_query_via_path_total counter\n";
    for (int i = 0; i < kViaPathCount; ++i) {
        os << "cortrix_bge_m3_sparse_query_via_path_total{path=\""
           << ToString(static_cast<ViaPath>(i)) << "\"} "
           << via_path_[i].load(std::memory_order_relaxed) << "\n";
    }
    return os.str();
}

void SparseMetrics::ResetForTest() {
    for (auto& a : inference_) a.store(0, std::memory_order_relaxed);
    for (auto& a : via_path_) a.store(0, std::memory_order_relaxed);
    fallback_ratio_bits_.store(0, std::memory_order_relaxed);
    inverted_index_size_bytes_.store(0, std::memory_order_relaxed);
}

}  // namespace cortrix::retrieval
