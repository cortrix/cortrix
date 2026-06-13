#include "cortrix/doc_summary/doc_summary_metrics.h"

#include <sstream>

namespace cortrix::doc_summary {

namespace {
// llm_duration_seconds histogram upper bounds (seconds).
constexpr double kDurBounds[DocSummaryMetrics::kDurBucketCount] = {
    0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0};
const char* kDurBoundStr[DocSummaryMetrics::kDurBucketCount] = {
    "0.1", "0.25", "0.5", "1", "2", "5", "10"};
}  // namespace

const char* ToString(DocSummaryMetrics::LlmCallStatus status) {
    switch (status) {
        case DocSummaryMetrics::LlmCallStatus::kStarted: return "started";
        case DocSummaryMetrics::LlmCallStatus::kSuccess: return "success";
        case DocSummaryMetrics::LlmCallStatus::kFailed:  return "failed";
    }
    return "started";
}

const char* ToString(DocSummaryMetrics::FallbackResult result) {
    switch (result) {
        case DocSummaryMetrics::FallbackResult::kHit:  return "hit";
        case DocSummaryMetrics::FallbackResult::kMiss: return "miss";
    }
    return "hit";
}

DocSummaryMetrics& DocSummaryMetrics::Instance() {
    static DocSummaryMetrics instance;
    return instance;
}

void DocSummaryMetrics::RecordLlmCall(LlmCallStatus status) {
    llm_calls_[static_cast<int>(status)].fetch_add(1, std::memory_order_relaxed);
}
uint64_t DocSummaryMetrics::LlmCallCount(LlmCallStatus status) const {
    return llm_calls_[static_cast<int>(status)].load(std::memory_order_relaxed);
}

void DocSummaryMetrics::ObserveLlmDuration(const std::string& model, double seconds) {
    std::lock_guard<std::mutex> lk(mu_);
    Histogram& h = llm_duration_[model];
    int b = kDurBucketCount;  // default +Inf
    for (int i = 0; i < kDurBucketCount; ++i) {
        if (seconds <= kDurBounds[i]) { b = i; break; }
    }
    h.buckets[b]++;
    h.sum += seconds;
    h.count++;
}
uint64_t DocSummaryMetrics::LlmDurationCount(const std::string& model) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = llm_duration_.find(model);
    return it == llm_duration_.end() ? 0 : it->second.count;
}
double DocSummaryMetrics::LlmDurationSum(const std::string& model) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = llm_duration_.find(model);
    return it == llm_duration_.end() ? 0.0 : it->second.sum;
}

void DocSummaryMetrics::AddSummariesGenerated(uint64_t n) {
    summaries_generated_.fetch_add(n, std::memory_order_relaxed);
}
uint64_t DocSummaryMetrics::SummariesGeneratedCount() const {
    return summaries_generated_.load(std::memory_order_relaxed);
}

void DocSummaryMetrics::RecordFallbackTriggered(FallbackResult result) {
    fallback_triggered_[static_cast<int>(result)].fetch_add(1, std::memory_order_relaxed);
}
uint64_t DocSummaryMetrics::FallbackTriggeredCount(FallbackResult result) const {
    return fallback_triggered_[static_cast<int>(result)].load(std::memory_order_relaxed);
}

void DocSummaryMetrics::RecordFts5FallbackFailed() {
    fts5_fallback_failed_.fetch_add(1, std::memory_order_relaxed);
}
uint64_t DocSummaryMetrics::Fts5FallbackFailedCount() const {
    return fts5_fallback_failed_.load(std::memory_order_relaxed);
}

std::string DocSummaryMetrics::RenderOpenMetrics() const {
    std::ostringstream os;
    os << "# TYPE cortrix_doc_summary_llm_calls_total counter\n";
    for (int i = 0; i < kLlmStatusCount; ++i) {
        os << "cortrix_doc_summary_llm_calls_total{status=\""
           << ToString(static_cast<LlmCallStatus>(i)) << "\"} "
           << llm_calls_[i].load(std::memory_order_relaxed) << "\n";
    }

    os << "# TYPE cortrix_doc_summary_summaries_generated_total counter\n";
    os << "cortrix_doc_summary_summaries_generated_total "
       << summaries_generated_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_doc_summary_fallback_triggered_total counter\n";
    for (int i = 0; i < kFallbackResultCount; ++i) {
        os << "cortrix_doc_summary_fallback_triggered_total{result=\""
           << ToString(static_cast<FallbackResult>(i)) << "\"} "
           << fallback_triggered_[i].load(std::memory_order_relaxed) << "\n";
    }

    os << "# TYPE cortrix_fts5_fallback_failed_total counter\n";
    os << "cortrix_fts5_fallback_failed_total "
       << fts5_fallback_failed_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_doc_summary_llm_duration_seconds histogram\n";
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [model, h] : llm_duration_) {
            uint64_t cum = 0;
            for (int i = 0; i < kDurBucketCount; ++i) {
                cum += h.buckets[i];
                os << "cortrix_doc_summary_llm_duration_seconds_bucket{model=\""
                   << model << "\",le=\"" << kDurBoundStr[i] << "\"} " << cum << "\n";
            }
            cum += h.buckets[kDurBucketCount];
            os << "cortrix_doc_summary_llm_duration_seconds_bucket{model=\"" << model
               << "\",le=\"+Inf\"} " << cum << "\n";
            os << "cortrix_doc_summary_llm_duration_seconds_sum{model=\"" << model
               << "\"} " << h.sum << "\n";
            os << "cortrix_doc_summary_llm_duration_seconds_count{model=\"" << model
               << "\"} " << h.count << "\n";
        }
    }
    return os.str();
}

void DocSummaryMetrics::ResetForTest() {
    for (auto& a : llm_calls_) a.store(0, std::memory_order_relaxed);
    for (auto& a : fallback_triggered_) a.store(0, std::memory_order_relaxed);
    summaries_generated_.store(0, std::memory_order_relaxed);
    fts5_fallback_failed_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mu_);
    llm_duration_.clear();
}

}  // namespace cortrix::doc_summary
