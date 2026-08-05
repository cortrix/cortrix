#include <cstdint>
#include "cortrix/query/query_router_metrics.h"

#include <sstream>

namespace cortrix::query {

namespace {

// classifier_latency_seconds `le` bounds (seconds). Rule path
// P50<0.5ms / P99<2ms; LLM path (Phase 2) P50<200ms / P99<800ms → fine sub-ms..ms
// buckets straddling the rule SLA, plus headroom up to the LLM range.
constexpr double kLatBounds[] = {0.0005, 0.002, 0.01, 0.05, 0.1, 0.2, 0.5, 1.0};
constexpr const char* kLatBoundStr[] = {"0.0005", "0.002", "0.01", "0.05",
                                        "0.1", "0.2", "0.5", "1.0"};

// Index of the first finite bucket whose bound ≥ value, else the +Inf slot.
template <size_t N>
int LeIndex(double value, const double (&bounds)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (value <= bounds[i]) return static_cast<int>(i);
    }
    return static_cast<int>(N);  // +Inf
}

// Fixed-point parts-per-million encoding for the ratio gauge (avoids atomic<double>).
constexpr double kPpm = 1'000'000.0;

}  // namespace

const char* ToString(QueryRouterMetrics::Decision decision) {
    switch (decision) {
        case QueryRouterMetrics::Decision::kSimple:   return "simple";
        case QueryRouterMetrics::Decision::kComplex:  return "complex";
        case QueryRouterMetrics::Decision::kChat:     return "chat";
        case QueryRouterMetrics::Decision::kFallback: return "fallback";
    }
    return "complex";
}

const char* ToString(QueryRouterMetrics::SavedPath path) {
    switch (path) {
        case QueryRouterMetrics::SavedPath::kSimple: return "simple";
        case QueryRouterMetrics::SavedPath::kChat:   return "chat";
    }
    return "simple";
}

QueryRouterMetrics& QueryRouterMetrics::Instance() {
    static QueryRouterMetrics instance;
    return instance;
}

void QueryRouterMetrics::RecordDecision(Decision decision) {
    decision_[static_cast<int>(decision)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t QueryRouterMetrics::DecisionCount(Decision decision) const {
    return decision_[static_cast<int>(decision)].load(std::memory_order_relaxed);
}

void QueryRouterMetrics::ObserveClassifierLatency(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const int idx = LeIndex(seconds, kLatBounds);
    lat_bkt_[idx].fetch_add(1, std::memory_order_relaxed);
    lat_sum_us_.fetch_add(static_cast<uint64_t>(seconds * 1'000'000.0),
                          std::memory_order_relaxed);
    lat_count_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t QueryRouterMetrics::ClassifierLatencyCount() const {
    return lat_count_.load(std::memory_order_relaxed);
}

double QueryRouterMetrics::ClassifierLatencySum() const {
    return static_cast<double>(lat_sum_us_.load(std::memory_order_relaxed)) / 1'000'000.0;
}

void QueryRouterMetrics::SetFallbackRatio(double ratio) {
    fallback_ratio_ppm_.store(static_cast<int64_t>(ratio * kPpm), std::memory_order_relaxed);
}

double QueryRouterMetrics::FallbackRatio() const {
    return static_cast<double>(fallback_ratio_ppm_.load(std::memory_order_relaxed)) / kPpm;
}

void QueryRouterMetrics::AddComputeSaved(SavedPath path, double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    compute_saved_us_[static_cast<int>(path)].fetch_add(
        static_cast<uint64_t>(seconds * 1'000'000.0), std::memory_order_relaxed);
}

double QueryRouterMetrics::ComputeSavedSeconds(SavedPath path) const {
    return static_cast<double>(
               compute_saved_us_[static_cast<int>(path)].load(std::memory_order_relaxed)) /
           1'000'000.0;
}

void QueryRouterMetrics::ResetForTest() {
    for (auto& c : decision_) c.store(0, std::memory_order_relaxed);
    for (auto& b : lat_bkt_) b.store(0, std::memory_order_relaxed);
    lat_sum_us_.store(0, std::memory_order_relaxed);
    lat_count_.store(0, std::memory_order_relaxed);
    fallback_ratio_ppm_.store(0, std::memory_order_relaxed);
    for (auto& s : compute_saved_us_) s.store(0, std::memory_order_relaxed);
}

std::string QueryRouterMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_query_router_total {decision}
    os << "# TYPE cortrix_query_router_total counter\n";
    for (int i = 0; i < kDecisionCount; ++i) {
        os << "cortrix_query_router_total{decision=\""
           << ToString(static_cast<Decision>(i)) << "\"} "
           << decision_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_query_router_classifier_latency_seconds (no labels, histogram)
    os << "# TYPE cortrix_query_router_classifier_latency_seconds histogram\n";
    {
        uint64_t cum = 0;
        for (int b = 0; b < kLatencyBuckets; ++b) {
            cum += lat_bkt_[b].load(std::memory_order_relaxed);
            os << "cortrix_query_router_classifier_latency_seconds_bucket{le=\""
               << kLatBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += lat_bkt_[kLatencyBuckets].load(std::memory_order_relaxed);
        os << "cortrix_query_router_classifier_latency_seconds_bucket{le=\"+Inf\"} "
           << cum << "\n";
        os << "cortrix_query_router_classifier_latency_seconds_sum "
           << ClassifierLatencySum() << "\n";
        os << "cortrix_query_router_classifier_latency_seconds_count "
           << lat_count_.load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_query_router_fallback_ratio (no labels, gauge)
    os << "# TYPE cortrix_query_router_fallback_ratio gauge\n";
    os << "cortrix_query_router_fallback_ratio " << FallbackRatio() << "\n";

    // cortrix_query_router_compute_saved_seconds {path}
    os << "# TYPE cortrix_query_router_compute_saved_seconds counter\n";
    for (int i = 0; i < kSavedPathCount; ++i) {
        os << "cortrix_query_router_compute_saved_seconds{path=\""
           << ToString(static_cast<SavedPath>(i)) << "\"} "
           << ComputeSavedSeconds(static_cast<SavedPath>(i)) << "\n";
    }

    return os.str();
}

}  // namespace cortrix::query
