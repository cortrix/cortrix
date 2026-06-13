#include "cortrix/spc_enricher/enricher_metrics.h"

#include <sstream>

namespace cortrix::spc {

namespace {

// batch_size_actual bucket bounds — topic 1.2 batch_size 1-32 (default 8); count
// distribution (unitless, per §3.6). Powers-of-two straddle the configurable range.
constexpr double kBatchBounds[6] = {1, 2, 4, 8, 16, 32};
constexpr const char* kBatchBoundStr[6] = {"1", "2", "4", "8", "16", "32"};

// score_duration bucket bounds (seconds) — generic latency buckets (OBS_SPEC §2.2 /
// briefing default; no sub-ms SLA — LLM calls span 0.1s..task_timeout 30s).
constexpr double kDurBounds[8] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 30};
constexpr const char* kDurBoundStr[8] = {"0.1", "0.25", "0.5", "1", "2", "5", "10", "30"};

// Cumulative histogram render (matches scoring_metrics.cpp RenderHist): per-bucket
// {le} cumulative, then +Inf, _sum, _count. `sum` is already in the metric's unit.
void RenderHist(std::ostringstream& os, const char* name,
                const std::atomic<uint64_t>* bkt, int n_bounds,
                const char* const* bound_str, double sum, uint64_t count) {
    uint64_t cum = 0;
    for (int i = 0; i < n_bounds; ++i) {
        cum += bkt[i].load(std::memory_order_relaxed);
        os << name << "_bucket{le=\"" << bound_str[i] << "\"} " << cum << "\n";
    }
    cum += bkt[n_bounds].load(std::memory_order_relaxed);  // +Inf bucket
    os << name << "_bucket{le=\"+Inf\"} " << cum << "\n";
    os << name << "_sum " << sum << "\n";
    os << name << "_count " << count << "\n";
}

}  // namespace

EnricherMetrics& EnricherMetrics::Instance() {
    static EnricherMetrics instance;
    return instance;
}

// --- circuit_breaker_state / trips (S3.4) ---

void EnricherMetrics::SetCircuitBreakerState(int state) {
    cb_state_.store(state, std::memory_order_relaxed);
}
int EnricherMetrics::CircuitBreakerState() const {
    return cb_state_.load(std::memory_order_relaxed);
}
void EnricherMetrics::RecordCircuitBreakerTrip() {
    cb_trips_.fetch_add(1, std::memory_order_relaxed);
}
uint64_t EnricherMetrics::CircuitBreakerTripsCount() const {
    return cb_trips_.load(std::memory_order_relaxed);
}

// --- failed_tasks_total (S3.3, label: reason = 6 error codes) ---

void EnricherMetrics::RecordFailedTask(EnricherErrorCode reason) {
    failed_tasks_[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
}
uint64_t EnricherMetrics::FailedTasksCount(EnricherErrorCode reason) const {
    return failed_tasks_[static_cast<int>(reason)].load(std::memory_order_relaxed);
}

// --- fallback_to_null_total (S3.4 / W4) ---

void EnricherMetrics::RecordFallbackToNull(FallbackReason reason) {
    fallback_to_null_[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
}
uint64_t EnricherMetrics::FallbackToNullCount(FallbackReason reason) const {
    return fallback_to_null_[static_cast<int>(reason)].load(std::memory_order_relaxed);
}

// --- endpoint_probe_failed_total (W4) ---

void EnricherMetrics::RecordEndpointProbeFailed() {
    endpoint_probe_failed_.fetch_add(1, std::memory_order_relaxed);
}
uint64_t EnricherMetrics::EndpointProbeFailedCount() const {
    return endpoint_probe_failed_.load(std::memory_order_relaxed);
}

// --- tokens_total / cost_usd_total (per-model, S3.4 / S4.1) ---

void EnricherMetrics::RecordTokens(const std::string& model, int64_t tokens) {
    std::lock_guard<std::mutex> lk(mu_);
    tokens_by_model_[model] += tokens;
}
int64_t EnricherMetrics::TokensCount(const std::string& model) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tokens_by_model_.find(model);
    return it == tokens_by_model_.end() ? 0 : it->second;
}

void EnricherMetrics::RecordCostMicroUsd(const std::string& model, int64_t micro_usd) {
    std::lock_guard<std::mutex> lk(mu_);
    cost_micro_usd_by_model_[model] += micro_usd;
}
int64_t EnricherMetrics::CostMicroUsd(const std::string& model) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cost_micro_usd_by_model_.find(model);
    return it == cost_micro_usd_by_model_.end() ? 0 : it->second;
}

// --- truncated_total / queue_depth_current (S3.3 / S3.1) ---

void EnricherMetrics::RecordTruncated() {
    truncated_.fetch_add(1, std::memory_order_relaxed);
}
uint64_t EnricherMetrics::TruncatedCount() const {
    return truncated_.load(std::memory_order_relaxed);
}
void EnricherMetrics::SetQueueDepth(int depth) {
    queue_depth_.store(depth, std::memory_order_relaxed);
}
int EnricherMetrics::QueueDepth() const {
    return queue_depth_.load(std::memory_order_relaxed);
}

// --- batch_size_actual (histogram, §3.6) ---

void EnricherMetrics::ObserveBatchSizeActual(int batch_size) {
    if (batch_size < 0) batch_size = 0;
    int i = kBatchBucketCount;  // +Inf bucket index
    for (int j = 0; j < kBatchBucketCount; ++j) {
        if (batch_size <= kBatchBounds[j]) { i = j; break; }
    }
    batch_bkt_[static_cast<size_t>(i)].fetch_add(1, std::memory_order_relaxed);
    batch_count_.fetch_add(1, std::memory_order_relaxed);
    batch_sum_.fetch_add(static_cast<uint64_t>(batch_size), std::memory_order_relaxed);
}
uint64_t EnricherMetrics::BatchSizeActualCount() const {
    return batch_count_.load(std::memory_order_relaxed);
}

// --- score_duration_seconds (histogram, §3.6) ---

void EnricherMetrics::ObserveScoreDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    int i = kDurBucketCount;  // +Inf bucket index
    for (int j = 0; j < kDurBucketCount; ++j) {
        if (seconds <= kDurBounds[j]) { i = j; break; }
    }
    dur_bkt_[static_cast<size_t>(i)].fetch_add(1, std::memory_order_relaxed);
    dur_count_.fetch_add(1, std::memory_order_relaxed);
    dur_sum_us_.fetch_add(static_cast<uint64_t>(seconds * 1e6), std::memory_order_relaxed);
}
uint64_t EnricherMetrics::ScoreDurationCount() const {
    return dur_count_.load(std::memory_order_relaxed);
}

std::string EnricherMetrics::RenderOpenMetrics() const {
    std::ostringstream os;
    os << "# TYPE cortrix_enricher_circuit_breaker_state gauge\n";
    os << "cortrix_enricher_circuit_breaker_state "
       << cb_state_.load(std::memory_order_relaxed) << "\n";
    os << "# TYPE cortrix_enricher_circuit_breaker_trips_total counter\n";
    os << "cortrix_enricher_circuit_breaker_trips_total "
       << cb_trips_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_enricher_failed_tasks_total counter\n";
    for (int i = 0; i < kEnricherErrorCodeCount; ++i) {
        os << "cortrix_enricher_failed_tasks_total{reason=\""
           << EnricherErrorCodeString(static_cast<EnricherErrorCode>(i)) << "\"} "
           << failed_tasks_[i].load(std::memory_order_relaxed) << "\n";
    }

    os << "# TYPE cortrix_enricher_fallback_to_null_total counter\n";
    for (int i = 0; i < kFallbackReasonCount; ++i) {
        os << "cortrix_enricher_fallback_to_null_total{reason=\""
           << ToString(static_cast<FallbackReason>(i)) << "\"} "
           << fallback_to_null_[i].load(std::memory_order_relaxed) << "\n";
    }

    os << "# TYPE cortrix_enricher_endpoint_probe_failed_total counter\n";
    os << "cortrix_enricher_endpoint_probe_failed_total "
       << endpoint_probe_failed_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_enricher_truncated_total counter\n";
    os << "cortrix_enricher_truncated_total "
       << truncated_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_enricher_queue_depth_current gauge\n";
    os << "cortrix_enricher_queue_depth_current "
       << queue_depth_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_enricher_batch_size_actual histogram\n";
    RenderHist(os, "cortrix_enricher_batch_size_actual", batch_bkt_.data(),
               kBatchBucketCount, kBatchBoundStr,
               static_cast<double>(batch_sum_.load(std::memory_order_relaxed)),
               batch_count_.load(std::memory_order_relaxed));

    os << "# TYPE cortrix_enricher_score_duration_seconds histogram\n";
    RenderHist(os, "cortrix_enricher_score_duration_seconds", dur_bkt_.data(),
               kDurBucketCount, kDurBoundStr,
               static_cast<double>(dur_sum_us_.load(std::memory_order_relaxed)) / 1e6,
               dur_count_.load(std::memory_order_relaxed));

    {
        std::lock_guard<std::mutex> lk(mu_);
        os << "# TYPE cortrix_enricher_tokens_total counter\n";
        for (const auto& [model, n] : tokens_by_model_) {
            os << "cortrix_enricher_tokens_total{model=\"" << model << "\"} " << n << "\n";
        }
        os << "# TYPE cortrix_enricher_cost_usd_total counter\n";
        for (const auto& [model, micro] : cost_micro_usd_by_model_) {
            // micro-USD → USD with 6 decimals.
            os << "cortrix_enricher_cost_usd_total{model=\"" << model << "\"} "
               << (static_cast<double>(micro) / 1e6) << "\n";
        }
    }
    return os.str();
}

void EnricherMetrics::ResetForTest() {
    cb_state_.store(0, std::memory_order_relaxed);
    cb_trips_.store(0, std::memory_order_relaxed);
    for (auto& c : failed_tasks_) c.store(0, std::memory_order_relaxed);
    for (auto& c : fallback_to_null_) c.store(0, std::memory_order_relaxed);
    endpoint_probe_failed_.store(0, std::memory_order_relaxed);
    truncated_.store(0, std::memory_order_relaxed);
    queue_depth_.store(0, std::memory_order_relaxed);
    for (auto& b : batch_bkt_) b.store(0, std::memory_order_relaxed);
    batch_sum_.store(0, std::memory_order_relaxed);
    batch_count_.store(0, std::memory_order_relaxed);
    for (auto& b : dur_bkt_) b.store(0, std::memory_order_relaxed);
    dur_sum_us_.store(0, std::memory_order_relaxed);
    dur_count_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mu_);
    tokens_by_model_.clear();
    cost_micro_usd_by_model_.clear();
}

const char* ToString(EnricherMetrics::FallbackReason reason) {
    switch (reason) {
        case EnricherMetrics::FallbackReason::kApiKeyMissing:       return "api_key_missing";
        case EnricherMetrics::FallbackReason::kEndpointUnreachable: return "endpoint_unreachable";
        case EnricherMetrics::FallbackReason::kBudgetExceeded:      return "budget_exceeded";
        case EnricherMetrics::FallbackReason::kCircuitOpen:         return "circuit_open";
    }
    return "unknown";
}

}  // namespace cortrix::spc
