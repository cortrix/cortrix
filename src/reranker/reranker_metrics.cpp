#include <cstdint>
#include "cortrix/reranker/reranker_metrics.h"

#include <cstring>
#include <sstream>

namespace cortrix::reranker {

namespace {

// cortrix_reranker_score_duration_seconds bucket upper bounds (seconds), aligned
// with RerankerMetrics::kScoreBucketCount. Chosen against the reranker SLA:
// component P99=200ms, 30-candidate ScoreBatch ~240ms, end-to-end P50<500ms /
// P99<1500ms — buckets sit on those thresholds with headroom to 5s.
constexpr double kScoreBounds[8] = {0.05, 0.1, 0.2, 0.5, 1.0, 1.5, 2.0, 5.0};
constexpr const char* kScoreBoundStr[8] = {
    "0.05", "0.1", "0.2", "0.5", "1", "1.5", "2", "5"};

// Pack/unpack a double through a uint64_t for the atomic sum accumulator (no
// portable atomic<double> arithmetic pre-C++20; one writer per ScoreBatch).
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

int EpCode(const std::string& ep) {
    if (ep == "cpu") return 1;
    if (ep == "coreml") return 2;
    if (ep == "cuda") return 3;
    if (ep == "stub") return 4;
    return 0;
}

const char* EpName(int code) {
    switch (code) {
        case 1: return "cpu";
        case 2: return "coreml";
        case 3: return "cuda";
        case 4: return "stub";
        default: return "auto";
    }
}

int ProviderFallbackIndex(const std::string& from,
                          RerankerMetrics::ProviderFallbackReason reason) {
    const int provider = from == "cuda" ? 1 : 0;
    return provider * 2 + static_cast<int>(reason);
}

int ProviderInitFailureIndex(const std::string& provider,
                             RerankerMetrics::ProviderFallbackReason reason) {
    int provider_index = -1;
    if (provider == "cpu") provider_index = 0;
    if (provider == "coreml") provider_index = 1;
    if (provider == "cuda") provider_index = 2;
    return provider_index < 0
        ? -1
        : provider_index * 2 + static_cast<int>(reason);
}

}  // namespace

RerankerMetrics& RerankerMetrics::Instance() {
    static RerankerMetrics instance;
    return instance;
}

// --- coreml_fallback_total (S1.4) ---

void RerankerMetrics::RecordCoremlFallback(CoremlFallbackReason reason) {
    coreml_fallback_[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t RerankerMetrics::CoremlFallbackCount(CoremlFallbackReason reason) const {
    return coreml_fallback_[static_cast<int>(reason)].load(std::memory_order_relaxed);
}

void RerankerMetrics::SetConfiguredEp(const std::string& ep) {
    configured_ep_.store(EpCode(ep), std::memory_order_relaxed);
}

std::string RerankerMetrics::ConfiguredEp() const {
    return EpName(configured_ep_.load(std::memory_order_relaxed));
}

void RerankerMetrics::RecordProviderFallback(
    const std::string& from, ProviderFallbackReason reason) {
    if (from != "coreml" && from != "cuda") return;
    provider_fallback_[ProviderFallbackIndex(from, reason)].fetch_add(
        1, std::memory_order_relaxed);
}

uint64_t RerankerMetrics::ProviderFallbackCount(
    const std::string& from, ProviderFallbackReason reason) const {
    if (from != "coreml" && from != "cuda") return 0;
    return provider_fallback_[ProviderFallbackIndex(from, reason)].load(
        std::memory_order_relaxed);
}

void RerankerMetrics::RecordProviderInitFailure(
    const std::string& provider, ProviderFallbackReason reason) {
    const int index = ProviderInitFailureIndex(provider, reason);
    if (index < 0) return;
    provider_init_failure_[index].fetch_add(1, std::memory_order_relaxed);
}

uint64_t RerankerMetrics::ProviderInitFailureCount(
    const std::string& provider, ProviderFallbackReason reason) const {
    const int index = ProviderInitFailureIndex(provider, reason);
    if (index < 0) return 0;
    return provider_init_failure_[index].load(std::memory_order_relaxed);
}

// --- active_ep (S1.4) ---

void RerankerMetrics::SetActiveEp(const std::string& ep) {
    active_ep_.store(EpCode(ep), std::memory_order_relaxed);
}

std::string RerankerMetrics::ActiveEp() const {
    return EpName(active_ep_.load(std::memory_order_relaxed));
}

// --- circuit_breaker_state / trips (S2.5) ---

void RerankerMetrics::SetCircuitBreakerState(int state) {
    cb_state_.store(state, std::memory_order_relaxed);
}

int RerankerMetrics::CircuitBreakerState() const {
    return cb_state_.load(std::memory_order_relaxed);
}

void RerankerMetrics::RecordCircuitBreakerTrip() {
    cb_trips_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t RerankerMetrics::CircuitBreakerTripsCount() const {
    return cb_trips_.load(std::memory_order_relaxed);
}

// --- failed_tasks_total (S2.5 / S3.4) ---

void RerankerMetrics::RecordFailedTask(FailedTaskReason reason) {
    failed_tasks_[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t RerankerMetrics::FailedTasksCount(FailedTaskReason reason) const {
    return failed_tasks_[static_cast<int>(reason)].load(std::memory_order_relaxed);
}

// --- truncated_total / extremely_long_total (S3.3) ---

void RerankerMetrics::RecordTruncated() {
    truncated_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t RerankerMetrics::TruncatedCount() const {
    return truncated_.load(std::memory_order_relaxed);
}

void RerankerMetrics::RecordExtremelyLong() {
    extremely_long_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t RerankerMetrics::ExtremelyLongCount() const {
    return extremely_long_.load(std::memory_order_relaxed);
}

// --- queue_depth_current / score_duration_seconds (MET-04) ---

void RerankerMetrics::SetQueueDepth(int depth) {
    if (depth < 0) depth = 0;
    queue_depth_.store(depth, std::memory_order_relaxed);
}

int RerankerMetrics::QueueDepth() const {
    return queue_depth_.load(std::memory_order_relaxed);
}

void RerankerMetrics::ObserveScoreDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    score_dur_count_.fetch_add(1, std::memory_order_relaxed);
    // CAS-accumulate the sum (low contention: one observe per ScoreBatch call).
    uint64_t cur = score_dur_sum_bits_.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        next = DoubleToBits(BitsToDouble(cur) + seconds);
    } while (!score_dur_sum_bits_.compare_exchange_weak(
        cur, next, std::memory_order_relaxed));
    // Land the sample in the smallest bucket whose le bound it fits (else +Inf).
    int idx = kScoreBucketCount;
    for (int j = 0; j < kScoreBucketCount; ++j) {
        if (seconds <= kScoreBounds[j]) { idx = j; break; }
    }
    score_dur_bkt_[idx].fetch_add(1, std::memory_order_relaxed);
}

uint64_t RerankerMetrics::ScoreDurationCount() const {
    return score_dur_count_.load(std::memory_order_relaxed);
}

double RerankerMetrics::ScoreDurationSum() const {
    return BitsToDouble(score_dur_sum_bits_.load(std::memory_order_relaxed));
}

std::string RerankerMetrics::RenderOpenMetrics() const {
    std::ostringstream os;
    os << "# TYPE cortrix_reranker_coreml_fallback_total counter\n";
    for (int i = 0; i < kCoremlReasonCount; ++i) {
        os << "cortrix_reranker_coreml_fallback_total{reason=\""
           << ToString(static_cast<CoremlFallbackReason>(i)) << "\"} "
           << coreml_fallback_[i].load(std::memory_order_relaxed) << "\n";
    }
    os << "# TYPE cortrix_reranker_active_ep gauge\n";
    os << "cortrix_reranker_active_ep{ep=\"" << ActiveEp() << "\"} 1\n";
    os << "# TYPE cortrix_reranker_configured_ep gauge\n";
    os << "cortrix_reranker_configured_ep{ep=\"" << ConfiguredEp() << "\"} 1\n";
    os << "# TYPE cortrix_reranker_ep_fallback_total counter\n";
    for (const char* from : {"coreml", "cuda"}) {
        for (int i = 0; i < 2; ++i) {
            const auto reason = static_cast<ProviderFallbackReason>(i);
            os << "cortrix_reranker_ep_fallback_total{from=\"" << from
               << "\",to=\"cpu\",reason=\"" << ToString(reason) << "\"} "
               << ProviderFallbackCount(from, reason) << "\n";
        }
    }
    os << "# TYPE cortrix_reranker_ep_init_failure_total counter\n";
    for (const char* provider : {"cpu", "coreml", "cuda"}) {
        for (int i = 0; i < 2; ++i) {
            const auto reason = static_cast<ProviderFallbackReason>(i);
            os << "cortrix_reranker_ep_init_failure_total{provider=\""
               << provider << "\",reason=\"" << ToString(reason) << "\"} "
               << ProviderInitFailureCount(provider, reason) << "\n";
        }
    }
    os << "# TYPE cortrix_reranker_circuit_breaker_state gauge\n";
    os << "cortrix_reranker_circuit_breaker_state "
       << cb_state_.load(std::memory_order_relaxed) << "\n";
    os << "# TYPE cortrix_reranker_circuit_breaker_trips_total counter\n";
    os << "cortrix_reranker_circuit_breaker_trips_total "
       << cb_trips_.load(std::memory_order_relaxed) << "\n";
    os << "# TYPE cortrix_reranker_failed_tasks_total counter\n";
    for (int i = 0; i < kFailedReasonCount; ++i) {
        os << "cortrix_reranker_failed_tasks_total{reason=\""
           << ToString(static_cast<FailedTaskReason>(i)) << "\"} "
           << failed_tasks_[i].load(std::memory_order_relaxed) << "\n";
    }
    os << "# TYPE cortrix_reranker_truncated_total counter\n";
    os << "cortrix_reranker_truncated_total "
       << truncated_.load(std::memory_order_relaxed) << "\n";
    os << "# TYPE cortrix_reranker_extremely_long_total counter\n";
    os << "cortrix_reranker_extremely_long_total "
       << extremely_long_.load(std::memory_order_relaxed) << "\n";
    // queue_depth_current gauge (MET-04).
    os << "# TYPE cortrix_reranker_queue_depth_current gauge\n";
    os << "cortrix_reranker_queue_depth_current "
       << queue_depth_.load(std::memory_order_relaxed) << "\n";
    // score_duration_seconds histogram (MET-04): cumulative le buckets + +Inf
    // + _sum + _count.
    os << "# TYPE cortrix_reranker_score_duration_seconds histogram\n";
    uint64_t sd_cum = 0;
    for (int i = 0; i < kScoreBucketCount; ++i) {
        sd_cum += score_dur_bkt_[i].load(std::memory_order_relaxed);
        os << "cortrix_reranker_score_duration_seconds_bucket{le=\""
           << kScoreBoundStr[i] << "\"} " << sd_cum << "\n";
    }
    sd_cum += score_dur_bkt_[kScoreBucketCount].load(std::memory_order_relaxed);
    os << "cortrix_reranker_score_duration_seconds_bucket{le=\"+Inf\"} " << sd_cum << "\n";
    os << "cortrix_reranker_score_duration_seconds_sum "
       << ScoreDurationSum() << "\n";
    os << "cortrix_reranker_score_duration_seconds_count "
       << score_dur_count_.load(std::memory_order_relaxed) << "\n";
    return os.str();
}

void RerankerMetrics::ResetForTest() {
    for (auto& c : coreml_fallback_) c.store(0, std::memory_order_relaxed);
    for (auto& c : provider_fallback_) c.store(0, std::memory_order_relaxed);
    for (auto& c : provider_init_failure_) c.store(0, std::memory_order_relaxed);
    for (auto& c : failed_tasks_) c.store(0, std::memory_order_relaxed);
    cb_state_.store(0, std::memory_order_relaxed);
    cb_trips_.store(0, std::memory_order_relaxed);
    truncated_.store(0, std::memory_order_relaxed);
    extremely_long_.store(0, std::memory_order_relaxed);
    configured_ep_.store(0, std::memory_order_relaxed);
    active_ep_.store(1, std::memory_order_relaxed);
    queue_depth_.store(0, std::memory_order_relaxed);
    for (auto& b : score_dur_bkt_) b.store(0, std::memory_order_relaxed);
    score_dur_sum_bits_.store(0, std::memory_order_relaxed);
    score_dur_count_.store(0, std::memory_order_relaxed);
}

const char* ToString(RerankerMetrics::CoremlFallbackReason reason) {
    switch (reason) {
        case RerankerMetrics::CoremlFallbackReason::kEpInitFailed:       return "ep_init_failed";
        case RerankerMetrics::CoremlFallbackReason::kUnsupportedPlatform: return "unsupported_platform";
    }
    return "unknown";
}

const char* ToString(RerankerMetrics::ProviderFallbackReason reason) {
    switch (reason) {
        case RerankerMetrics::ProviderFallbackReason::kEpInitFailed:
            return "ep_init_failed";
        case RerankerMetrics::ProviderFallbackReason::kSessionInitFailed:
            return "session_init_failed";
    }
    return "unknown";
}

const char* ToString(RerankerMetrics::FailedTaskReason reason) {
    switch (reason) {
        case RerankerMetrics::FailedTaskReason::kOnnxException: return "onnx_exception";
        case RerankerMetrics::FailedTaskReason::kOom:           return "oom";
        case RerankerMetrics::FailedTaskReason::kTimeout:       return "timeout";
    }
    return "unknown";
}

}  // namespace cortrix::reranker
