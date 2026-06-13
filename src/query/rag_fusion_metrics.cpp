#include "cortrix/query/rag_fusion_metrics.h"

#include <sstream>

namespace cortrix::query {

namespace {

// variant_count `le` bounds — count-type (§topic-1 N=3 default, NS-adjustable [1-10]).
constexpr double kVariantBounds[] = {1, 2, 3, 5, 10};
constexpr const char* kVariantBoundStr[] = {"1", "2", "3", "5", "10"};

// llm_latency_seconds `le` bounds (seconds) — generic duration set (§4.2
// timeout_ms default 5000, typical ~500ms).
constexpr double kLatBounds[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 30};
constexpr const char* kLatBoundStr[] = {"0.1", "0.25", "0.5", "1", "2", "5", "10", "30"};

// rrf_fusion_duration_seconds `le` bounds (seconds) — sub-ms..ms in-memory fusion.
constexpr double kRrfBounds[] = {0.0001, 0.001, 0.01, 0.1, 1};
constexpr const char* kRrfBoundStr[] = {"0.0001", "0.001", "0.01", "0.1", "1"};

// Index of the first finite bucket whose bound ≥ value, else the +Inf slot.
template <size_t N>
int LeIndex(double value, const double (&bounds)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (value <= bounds[i]) return static_cast<int>(i);
    }
    return static_cast<int>(N);  // +Inf
}

}  // namespace

const char* ToString(RagFusionMetrics::Result result) {
    switch (result) {
        case RagFusionMetrics::Result::kSuccess:  return "success";
        case RagFusionMetrics::Result::kDegraded: return "degraded";
        case RagFusionMetrics::Result::kDisabled: return "disabled";
    }
    return "success";
}

const char* ToString(RagFusionMetrics::DegradeReason reason) {
    switch (reason) {
        case RagFusionMetrics::DegradeReason::kLlmTimeout:      return "llm_timeout";
        case RagFusionMetrics::DegradeReason::kCircuitOpen:     return "circuit_open";
        case RagFusionMetrics::DegradeReason::kQuotaExceeded:   return "quota_exceeded";
        case RagFusionMetrics::DegradeReason::kInvalidResponse: return "invalid_response";
        case RagFusionMetrics::DegradeReason::kOther:           return "other";
    }
    return "other";
}

const char* ToString(RagFusionMetrics::TokenDirection direction) {
    switch (direction) {
        case RagFusionMetrics::TokenDirection::kInput:  return "input";
        case RagFusionMetrics::TokenDirection::kOutput: return "output";
    }
    return "input";
}

RagFusionMetrics& RagFusionMetrics::Instance() {
    static RagFusionMetrics instance;
    return instance;
}

int RagFusionMetrics::StrategyIdx(VariantStrategy s) {
    switch (s) {
        case VariantStrategy::kParaphrase: return 0;
        case VariantStrategy::kSubquery:   return 1;
        case VariantStrategy::kReverse:    return 2;
    }
    return 0;
}

void RagFusionMetrics::RecordInvocation(Result result) {
    invocation_[static_cast<int>(result)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t RagFusionMetrics::InvocationCount(Result result) const {
    return invocation_[static_cast<int>(result)].load(std::memory_order_relaxed);
}

void RagFusionMetrics::ObserveVariantCount(VariantStrategy strategy, int count) {
    if (count < 0) count = 0;
    const int i = StrategyIdx(strategy);
    variant_sum_[i].fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
    variant_count_[i].fetch_add(1, std::memory_order_relaxed);
    variant_bkt_[i][LeIndex(count, kVariantBounds)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t RagFusionMetrics::VariantCountSum(VariantStrategy strategy) const {
    return variant_sum_[StrategyIdx(strategy)].load(std::memory_order_relaxed);
}

uint64_t RagFusionMetrics::VariantCountObservations(VariantStrategy strategy) const {
    return variant_count_[StrategyIdx(strategy)].load(std::memory_order_relaxed);
}

uint64_t RagFusionMetrics::VariantCountBucket(VariantStrategy strategy, int le_index) const {
    if (le_index < 0 || le_index > kVariantBuckets) return 0;
    return variant_bkt_[StrategyIdx(strategy)][le_index].load(std::memory_order_relaxed);
}

void RagFusionMetrics::ObserveLlmLatency(const std::string& model, int latency_ms) {
    if (latency_ms < 0) latency_ms = 0;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(model_mu_);
        // Find existing slot or the first empty one.
        int empty = -1;
        for (int i = 0; i < kModelSlots; ++i) {
            if (model_names_[i] == model) { slot = i; break; }
            if (empty < 0 && model_names_[i].empty()) empty = i;
        }
        if (slot < 0) {
            // New model: take an empty slot, or fold into the last slot as the
            // shared "other" bucket once the (low-cardinality, §8 < 50) table fills.
            slot = (empty >= 0) ? empty : (kModelSlots - 1);
            if (empty >= 0) model_names_[slot] = model;
        }
    }
    model_latency_sum_ms_[slot].fetch_add(static_cast<uint64_t>(latency_ms),
                                          std::memory_order_relaxed);
    model_latency_count_[slot].fetch_add(1, std::memory_order_relaxed);
    model_latency_bkt_[slot][LeIndex(latency_ms / 1000.0, kLatBounds)].fetch_add(
        1, std::memory_order_relaxed);
}

void RagFusionMetrics::RecordDegraded(DegradeReason reason) {
    degraded_[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t RagFusionMetrics::DegradedCount(DegradeReason reason) const {
    return degraded_[static_cast<int>(reason)].load(std::memory_order_relaxed);
}

void RagFusionMetrics::RecordTokens(TokenDirection direction, uint64_t tokens) {
    token_[static_cast<int>(direction)].fetch_add(tokens, std::memory_order_relaxed);
}

uint64_t RagFusionMetrics::TokenCount(TokenDirection direction) const {
    return token_[static_cast<int>(direction)].load(std::memory_order_relaxed);
}

void RagFusionMetrics::ObserveRrfFusionDuration(int latency_us) {
    if (latency_us < 0) latency_us = 0;
    rrf_sum_us_.fetch_add(static_cast<uint64_t>(latency_us), std::memory_order_relaxed);
    rrf_count_.fetch_add(1, std::memory_order_relaxed);
    rrf_bkt_[LeIndex(latency_us / 1e6, kRrfBounds)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t RagFusionMetrics::RrfFusionDurationSumUs() const {
    return rrf_sum_us_.load(std::memory_order_relaxed);
}

uint64_t RagFusionMetrics::RrfFusionDurationCount() const {
    return rrf_count_.load(std::memory_order_relaxed);
}

uint64_t RagFusionMetrics::RrfFusionDurationBucket(int le_index) const {
    if (le_index < 0 || le_index > kRrfBuckets) return 0;
    return rrf_bkt_[le_index].load(std::memory_order_relaxed);
}

std::string RagFusionMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_rag_fusion_invocation_total {result}
    os << "# TYPE cortrix_rag_fusion_invocation_total counter\n";
    for (int i = 0; i < kResultCount; ++i) {
        os << "cortrix_rag_fusion_invocation_total{result=\""
           << ToString(static_cast<Result>(i)) << "\"} "
           << invocation_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_rag_fusion_variant_count {strategy} (histogram → _bucket / _sum / _count)
    os << "# TYPE cortrix_rag_fusion_variant_count histogram\n";
    const VariantStrategy kStrategies[kStrategyCount] = {
        VariantStrategy::kParaphrase, VariantStrategy::kSubquery, VariantStrategy::kReverse};
    for (VariantStrategy s : kStrategies) {
        const int i = StrategyIdx(s);
        // Label order matches the codebase convention (other label first, le last —
        // cf. import_metrics RenderHist).
        uint64_t cum = 0;
        for (int b = 0; b < kVariantBuckets; ++b) {
            cum += variant_bkt_[i][b].load(std::memory_order_relaxed);
            os << "cortrix_rag_fusion_variant_count_bucket{strategy=\"" << VariantStrategyString(s)
               << "\",le=\"" << kVariantBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += variant_bkt_[i][kVariantBuckets].load(std::memory_order_relaxed);
        os << "cortrix_rag_fusion_variant_count_bucket{strategy=\"" << VariantStrategyString(s)
           << "\",le=\"+Inf\"} " << cum << "\n";
        os << "cortrix_rag_fusion_variant_count_sum{strategy=\"" << VariantStrategyString(s)
           << "\"} " << variant_sum_[i].load(std::memory_order_relaxed) << "\n";
        os << "cortrix_rag_fusion_variant_count_count{strategy=\"" << VariantStrategyString(s)
           << "\"} " << variant_count_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_rag_fusion_llm_latency_seconds {model} (histogram → _bucket / _sum / _count)
    os << "# TYPE cortrix_rag_fusion_llm_latency_seconds histogram\n";
    {
        std::lock_guard<std::mutex> lock(model_mu_);
        for (int i = 0; i < kModelSlots; ++i) {
            const std::string& m = model_names_[i];
            const uint64_t cnt = model_latency_count_[i].load(std::memory_order_relaxed);
            if (m.empty() && cnt == 0) continue;  // unused slot
            const std::string label = m.empty() ? "other" : m;
            // Label order matches the codebase convention (model first, le last —
            // cf. import_metrics RenderHist / mem02 llm latency histogram).
            uint64_t cum = 0;
            for (int b = 0; b < kLatencyBuckets; ++b) {
                cum += model_latency_bkt_[i][b].load(std::memory_order_relaxed);
                os << "cortrix_rag_fusion_llm_latency_seconds_bucket{model=\"" << label
                   << "\",le=\"" << kLatBoundStr[b] << "\"} " << cum << "\n";
            }
            cum += model_latency_bkt_[i][kLatencyBuckets].load(std::memory_order_relaxed);
            os << "cortrix_rag_fusion_llm_latency_seconds_bucket{model=\"" << label
               << "\",le=\"+Inf\"} " << cum << "\n";
            const double sum_s =
                static_cast<double>(model_latency_sum_ms_[i].load(std::memory_order_relaxed)) / 1000.0;
            os << "cortrix_rag_fusion_llm_latency_seconds_sum{model=\"" << label << "\"} "
               << sum_s << "\n";
            os << "cortrix_rag_fusion_llm_latency_seconds_count{model=\"" << label << "\"} "
               << cnt << "\n";
        }
    }

    // cortrix_rag_fusion_degraded_total {reason}
    os << "# TYPE cortrix_rag_fusion_degraded_total counter\n";
    for (int i = 0; i < kDegradeReasonCount; ++i) {
        os << "cortrix_rag_fusion_degraded_total{reason=\""
           << ToString(static_cast<DegradeReason>(i)) << "\"} "
           << degraded_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_rag_fusion_token_total {direction}
    os << "# TYPE cortrix_rag_fusion_token_total counter\n";
    for (int i = 0; i < kDirectionCount; ++i) {
        os << "cortrix_rag_fusion_token_total{direction=\""
           << ToString(static_cast<TokenDirection>(i)) << "\"} "
           << token_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_rag_fusion_rrf_fusion_duration_seconds (no labels, histogram)
    os << "# TYPE cortrix_rag_fusion_rrf_fusion_duration_seconds histogram\n";
    {
        uint64_t cum = 0;
        for (int b = 0; b < kRrfBuckets; ++b) {
            cum += rrf_bkt_[b].load(std::memory_order_relaxed);
            os << "cortrix_rag_fusion_rrf_fusion_duration_seconds_bucket{le=\""
               << kRrfBoundStr[b] << "\"} " << cum << "\n";
        }
        cum += rrf_bkt_[kRrfBuckets].load(std::memory_order_relaxed);
        os << "cortrix_rag_fusion_rrf_fusion_duration_seconds_bucket{le=\"+Inf\"} " << cum
           << "\n";
    }
    os << "cortrix_rag_fusion_rrf_fusion_duration_seconds_sum "
       << (static_cast<double>(rrf_sum_us_.load(std::memory_order_relaxed)) / 1e6) << "\n";
    os << "cortrix_rag_fusion_rrf_fusion_duration_seconds_count "
       << rrf_count_.load(std::memory_order_relaxed) << "\n";

    return os.str();
}

void RagFusionMetrics::ResetForTest() {
    for (auto& a : invocation_) a.store(0, std::memory_order_relaxed);
    for (auto& a : variant_sum_) a.store(0, std::memory_order_relaxed);
    for (auto& a : variant_count_) a.store(0, std::memory_order_relaxed);
    for (auto& series : variant_bkt_)
        for (auto& a : series) a.store(0, std::memory_order_relaxed);
    for (auto& a : degraded_) a.store(0, std::memory_order_relaxed);
    for (auto& a : token_) a.store(0, std::memory_order_relaxed);
    rrf_sum_us_.store(0, std::memory_order_relaxed);
    rrf_count_.store(0, std::memory_order_relaxed);
    for (auto& a : rrf_bkt_) a.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(model_mu_);
    for (int i = 0; i < kModelSlots; ++i) {
        model_names_[i].clear();
        model_latency_sum_ms_[i].store(0, std::memory_order_relaxed);
        model_latency_count_[i].store(0, std::memory_order_relaxed);
        for (auto& a : model_latency_bkt_[i]) a.store(0, std::memory_order_relaxed);
    }
}

}  // namespace cortrix::query
