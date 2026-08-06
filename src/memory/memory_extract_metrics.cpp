#include <cstdint>
#include "cortrix/memory/memory_extract_metrics.h"

#include <sstream>

namespace cortrix::memory {

namespace {

// extract_duration_seconds histogram bucket upper bounds (seconds), Parallel
// string array for rendering exact `le` labels. The trailing +Inf bucket is implicit
// (index kNumDurBuckets). Mirrors the import_metrics kDurBounds/kDurBoundStr template.
constexpr double kDurBounds[MemoryExtractMetrics::kNumDurBuckets] =
    {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0};
constexpr const char* kDurBoundStr[MemoryExtractMetrics::kNumDurBuckets] =
    {"0.1", "0.25", "0.5", "1", "2", "5", "10", "30"};

// Index of the bucket a `seconds` observation falls into (kNumDurBuckets = +Inf).
int DurBucketIndex(double seconds) {
    for (int j = 0; j < MemoryExtractMetrics::kNumDurBuckets; ++j) {
        if (seconds <= kDurBounds[j]) return j;
    }
    return MemoryExtractMetrics::kNumDurBuckets;  // +Inf
}

}  // namespace

MemoryExtractMetrics::ConfidenceBucket MemoryExtractMetrics::BucketForConfidence(double confidence) {
    if (confidence >= 0.8) return ConfidenceBucket::kHigh;
    if (confidence >= 0.5) return ConfidenceBucket::kMedium;
    return ConfidenceBucket::kLow;
}

MemoryExtractMetrics& MemoryExtractMetrics::Instance() {
    static MemoryExtractMetrics instance;
    return instance;
}

int MemoryExtractMetrics::ModelSlotLocked(const std::string& model) {
    // Reserve the final slot as the shared overflow bucket. Linear scan over a
    // tiny table is fine (kModelSlots is small + this is off the lock-free path).
    for (int i = 0; i < kModelSlots - 1; ++i) {
        if (model_names_[i] == model) return i;
        if (model_names_[i].empty()) {
            model_names_[i] = model;
            return i;
        }
    }
    return kModelSlots - 1;  // overflow
}

void MemoryExtractMetrics::RecordExtract(ExtractStatus status) {
    extract_[static_cast<int>(status)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryExtractMetrics::ExtractCount(ExtractStatus status) const {
    return extract_[static_cast<int>(status)].load(std::memory_order_relaxed);
}

void MemoryExtractMetrics::ObserveExtractDuration(const std::string& model, int latency_ms) {
    if (latency_ms < 0) latency_ms = 0;
    int slot;
    {
        std::lock_guard<std::mutex> lk(model_mu_);
        slot = ModelSlotLocked(model);
    }
    model_latency_sum_ms_[slot].fetch_add(static_cast<uint64_t>(latency_ms),
                                          std::memory_order_relaxed);
    model_latency_count_[slot].fetch_add(1, std::memory_order_relaxed);
    // Histogram: bump the (non-cumulative) bucket this observation falls into.
    const int bi = DurBucketIndex(static_cast<double>(latency_ms) / 1000.0);
    model_latency_bkt_[slot][bi].fetch_add(1, std::memory_order_relaxed);
}

void MemoryExtractMetrics::SetQueueDepth(int64_t depth) {
    if (depth < 0) depth = 0;
    queue_depth_.store(depth, std::memory_order_relaxed);
}

int64_t MemoryExtractMetrics::QueueDepth() const {
    return queue_depth_.load(std::memory_order_relaxed);
}

void MemoryExtractMetrics::RecordTokens(const std::string& model, TokenDirection direction,
                                uint64_t tokens) {
    int slot;
    {
        std::lock_guard<std::mutex> lk(model_mu_);
        slot = ModelSlotLocked(model);
    }
    if (direction == TokenDirection::kInput) {
        model_token_in_[slot].fetch_add(tokens, std::memory_order_relaxed);
    } else {
        model_token_out_[slot].fetch_add(tokens, std::memory_order_relaxed);
    }
    token_[static_cast<int>(direction)].fetch_add(tokens, std::memory_order_relaxed);
}

uint64_t MemoryExtractMetrics::TokenCount(TokenDirection direction) const {
    return token_[static_cast<int>(direction)].load(std::memory_order_relaxed);
}

void MemoryExtractMetrics::RecordContradiction(ConfidenceBucket bucket) {
    contradiction_[static_cast<int>(bucket)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryExtractMetrics::ContradictionCount(ConfidenceBucket bucket) const {
    return contradiction_[static_cast<int>(bucket)].load(std::memory_order_relaxed);
}

void MemoryExtractMetrics::RecordInvalidation(TriggeredBy triggered_by) {
    invalidation_[static_cast<int>(triggered_by)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryExtractMetrics::InvalidationCount(TriggeredBy triggered_by) const {
    return invalidation_[static_cast<int>(triggered_by)].load(std::memory_order_relaxed);
}

std::string MemoryExtractMetrics::RenderOpenMetrics() const {
    std::ostringstream os;

    // cortrix_memory_extract_total
    os << "# HELP cortrix_memory_extract_total Total memory extractions by status.\n";
    os << "# TYPE cortrix_memory_extract_total counter\n";
    for (int i = 0; i < kExtractStatusCount; ++i) {
        os << "cortrix_memory_extract_total{status=\""
           << ToString(static_cast<ExtractStatus>(i)) << "\"} "
           << extract_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_memory_extract_duration_seconds — proper Prometheus histogram per model:
    // cumulative _bucket{le=...} for each bound + le="+Inf", then _sum + _count.
    os << "# HELP cortrix_memory_extract_duration_seconds Extraction latency in seconds.\n";
    os << "# TYPE cortrix_memory_extract_duration_seconds histogram\n";
    {
        std::lock_guard<std::mutex> lk(model_mu_);
        for (int i = 0; i < kModelSlots; ++i) {
            const uint64_t cnt = model_latency_count_[i].load(std::memory_order_relaxed);
            if (cnt == 0) continue;
            const std::string label = model_names_[i].empty() ? "other" : model_names_[i];
            // Cumulative buckets (non-cumulative storage summed up to each le).
            uint64_t cum = 0;
            for (int b = 0; b < kNumDurBuckets; ++b) {
                cum += model_latency_bkt_[i][b].load(std::memory_order_relaxed);
                os << "cortrix_memory_extract_duration_seconds_bucket{model=\"" << label
                   << "\",le=\"" << kDurBoundStr[b] << "\"} " << cum << "\n";
            }
            cum += model_latency_bkt_[i][kNumDurBuckets].load(std::memory_order_relaxed);
            os << "cortrix_memory_extract_duration_seconds_bucket{model=\"" << label
               << "\",le=\"+Inf\"} " << cum << "\n";
            const double sum_s =
                static_cast<double>(model_latency_sum_ms_[i].load(std::memory_order_relaxed)) / 1000.0;
            os << "cortrix_memory_extract_duration_seconds_sum{model=\"" << label << "\"} "
               << sum_s << "\n";
            os << "cortrix_memory_extract_duration_seconds_count{model=\"" << label << "\"} "
               << cnt << "\n";
        }
    }

    // cortrix_memory_extract_queue_depth
    os << "# HELP cortrix_memory_extract_queue_depth Current async extraction queue depth.\n";
    os << "# TYPE cortrix_memory_extract_queue_depth gauge\n";
    os << "cortrix_memory_extract_queue_depth " << queue_depth_.load(std::memory_order_relaxed) << "\n";

    // cortrix_memory_extract_llm_tokens_total (per model + direction)
    os << "# HELP cortrix_memory_extract_llm_tokens_total LLM tokens consumed by extraction.\n";
    os << "# TYPE cortrix_memory_extract_llm_tokens_total counter\n";
    {
        std::lock_guard<std::mutex> lk(model_mu_);
        for (int i = 0; i < kModelSlots; ++i) {
            const uint64_t in = model_token_in_[i].load(std::memory_order_relaxed);
            const uint64_t out = model_token_out_[i].load(std::memory_order_relaxed);
            if (in == 0 && out == 0) continue;
            const std::string label = model_names_[i].empty() ? "other" : model_names_[i];
            os << "cortrix_memory_extract_llm_tokens_total{model=\"" << label
               << "\",direction=\"input\"} " << in << "\n";
            os << "cortrix_memory_extract_llm_tokens_total{model=\"" << label
               << "\",direction=\"output\"} " << out << "\n";
        }
    }

    // cortrix_memory_extract_contradictions_found_total
    os << "# HELP cortrix_memory_extract_contradictions_found_total Contradictions found by confidence.\n";
    os << "# TYPE cortrix_memory_extract_contradictions_found_total counter\n";
    for (int i = 0; i < kConfidenceBucketCount; ++i) {
        os << "cortrix_memory_extract_contradictions_found_total{confidence_bucket=\""
           << ToString(static_cast<ConfidenceBucket>(i)) << "\"} "
           << contradiction_[i].load(std::memory_order_relaxed) << "\n";
    }

    // cortrix_memory_extract_invalidations_total
    os << "# HELP cortrix_memory_extract_invalidations_total Memory invalidations by trigger.\n";
    os << "# TYPE cortrix_memory_extract_invalidations_total counter\n";
    for (int i = 0; i < kTriggeredByCount; ++i) {
        os << "cortrix_memory_extract_invalidations_total{triggered_by=\""
           << ToString(static_cast<TriggeredBy>(i)) << "\"} "
           << invalidation_[i].load(std::memory_order_relaxed) << "\n";
    }

    return os.str();
}

void MemoryExtractMetrics::ResetForTest() {
    for (auto& a : extract_) a.store(0, std::memory_order_relaxed);
    for (auto& a : token_) a.store(0, std::memory_order_relaxed);
    for (auto& a : contradiction_) a.store(0, std::memory_order_relaxed);
    for (auto& a : invalidation_) a.store(0, std::memory_order_relaxed);
    queue_depth_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(model_mu_);
    for (int i = 0; i < kModelSlots; ++i) {
        model_names_[i].clear();
        model_latency_sum_ms_[i].store(0, std::memory_order_relaxed);
        model_latency_count_[i].store(0, std::memory_order_relaxed);
        for (auto& b : model_latency_bkt_[i]) b.store(0, std::memory_order_relaxed);
        model_token_in_[i].store(0, std::memory_order_relaxed);
        model_token_out_[i].store(0, std::memory_order_relaxed);
    }
}

const char* ToString(MemoryExtractMetrics::ExtractStatus status) {
    switch (status) {
        case MemoryExtractMetrics::ExtractStatus::kSuccess: return "success";
        case MemoryExtractMetrics::ExtractStatus::kFailed:  return "failed";
    }
    return "unknown";
}

const char* ToString(MemoryExtractMetrics::TokenDirection direction) {
    switch (direction) {
        case MemoryExtractMetrics::TokenDirection::kInput:  return "input";
        case MemoryExtractMetrics::TokenDirection::kOutput: return "output";
    }
    return "unknown";
}

const char* ToString(MemoryExtractMetrics::ConfidenceBucket bucket) {
    switch (bucket) {
        case MemoryExtractMetrics::ConfidenceBucket::kHigh:   return "high";
        case MemoryExtractMetrics::ConfidenceBucket::kMedium: return "medium";
        case MemoryExtractMetrics::ConfidenceBucket::kLow:    return "low";
    }
    return "unknown";
}

const char* ToString(MemoryExtractMetrics::TriggeredBy triggered_by) {
    switch (triggered_by) {
        case MemoryExtractMetrics::TriggeredBy::kLlmAuto:   return "llm_auto";
        case MemoryExtractMetrics::TriggeredBy::kManual:    return "manual";
        case MemoryExtractMetrics::TriggeredBy::kAgentSelf: return "agent_self";
    }
    return "unknown";
}

}  // namespace cortrix::memory
