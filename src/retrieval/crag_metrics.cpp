#include <cstdint>
#include "cortrix/retrieval/crag_metrics.h"

#include <sstream>

namespace cortrix::retrieval {

namespace {

// classifier_latency_seconds `le` bounds (seconds). SLA: P50<5ms /
// P99<20ms → fine sub-ms..ms buckets straddling the SLA, plus headroom.
constexpr double kLatBounds[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.5};
constexpr const char* kLatBoundStr[] = {"0.001", "0.002", "0.005", "0.01",
                                        "0.02", "0.05", "0.1", "0.5"};

// Index of the first finite bucket whose bound ≥ value, else the +Inf slot.
template <size_t N>
int LeIndex(double value, const double (&bounds)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (value <= bounds[i]) return static_cast<int>(i);
    }
    return static_cast<int>(N);  // +Inf
}

// Fixed-point parts-per-million encoding for the ratio gauges (avoids atomic<double>).
constexpr double kPpm = 1'000'000.0;

}  // namespace

const char* ToString(CragMetrics::Decision decision) {
    switch (decision) {
        case CragMetrics::Decision::kCorrect:   return "correct";
        case CragMetrics::Decision::kAmbiguous: return "ambiguous";
        case CragMetrics::Decision::kIncorrect: return "incorrect";
        case CragMetrics::Decision::kFallback:  return "fallback";
    }
    return "correct";
}

CragMetrics& CragMetrics::Instance() {
    static CragMetrics instance;
    return instance;
}

void CragMetrics::RecordEvaluation(Decision decision) {
    evaluation_[static_cast<int>(decision)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t CragMetrics::EvaluationCount(Decision decision) const {
    return evaluation_[static_cast<int>(decision)].load(std::memory_order_relaxed);
}

void CragMetrics::ObserveClassifierLatency(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const int idx = LeIndex(seconds, kLatBounds);
    lat_bkt_[idx].fetch_add(1, std::memory_order_relaxed);
    lat_sum_us_.fetch_add(static_cast<uint64_t>(seconds * 1'000'000.0),
                          std::memory_order_relaxed);
    lat_count_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t CragMetrics::ClassifierLatencyCount() const {
    return lat_count_.load(std::memory_order_relaxed);
}

double CragMetrics::ClassifierLatencySum() const {
    return static_cast<double>(lat_sum_us_.load(std::memory_order_relaxed)) / 1'000'000.0;
}

void CragMetrics::SetFallbackRatio(double ratio) {
    fallback_ratio_ppm_.store(static_cast<int64_t>(ratio * kPpm), std::memory_order_relaxed);
}

double CragMetrics::FallbackRatio() const {
    return static_cast<double>(fallback_ratio_ppm_.load(std::memory_order_relaxed)) / kPpm;
}

void CragMetrics::SetIncorrectRatio(double ratio) {
    incorrect_ratio_ppm_.store(static_cast<int64_t>(ratio * kPpm), std::memory_order_relaxed);
}

double CragMetrics::IncorrectRatio() const {
    return static_cast<double>(incorrect_ratio_ppm_.load(std::memory_order_relaxed)) / kPpm;
}

void CragMetrics::ResetForTest() {
    for (auto& c : evaluation_) c.store(0, std::memory_order_relaxed);
    for (auto& b : lat_bkt_) b.store(0, std::memory_order_relaxed);
    lat_sum_us_.store(0, std::memory_order_relaxed);
    lat_count_.store(0, std::memory_order_relaxed);
    fallback_ratio_ppm_.store(0, std::memory_order_relaxed);
    incorrect_ratio_ppm_.store(0, std::memory_order_relaxed);
}

std::string CragMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_crag_evaluation_total {decision}
    os << "# TYPE cortrix_crag_evaluation_total counter\n";
    for (int i = 0; i < kDecisionCount; ++i) {
        os << "cortrix_crag_evaluation_total{decision=\""
           << ToString(static_cast<Decision>(i)) << "\"} "
           << evaluation_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_crag_classifier_latency_seconds (no labels, histogram)
    os << "# TYPE cortrix_crag_classifier_latency_seconds histogram\n";
    {
        uint64_t cum = 0;
        for (int b = 0; b < kLatencyBuckets; ++b) {
            cum += lat_bkt_[b].load(std::memory_order_relaxed);
            os << "cortrix_crag_classifier_latency_seconds_bucket{le=\""
               << kLatBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += lat_bkt_[kLatencyBuckets].load(std::memory_order_relaxed);
        os << "cortrix_crag_classifier_latency_seconds_bucket{le=\"+Inf\"} " << cum << "\n";
        os << "cortrix_crag_classifier_latency_seconds_sum "
           << ClassifierLatencySum() << "\n";
        os << "cortrix_crag_classifier_latency_seconds_count "
           << lat_count_.load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_crag_fallback_ratio (no labels, gauge)
    os << "# TYPE cortrix_crag_fallback_ratio gauge\n";
    os << "cortrix_crag_fallback_ratio " << FallbackRatio() << "\n";

    // cortrix_crag_incorrect_ratio (no labels, gauge)
    os << "# TYPE cortrix_crag_incorrect_ratio gauge\n";
    os << "cortrix_crag_incorrect_ratio " << IncorrectRatio() << "\n";

    return os.str();
}

}  // namespace cortrix::retrieval
