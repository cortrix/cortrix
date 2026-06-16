#include <cstdint>
#include "cortrix/spc/hype_metrics.h"

#include <sstream>

namespace cortrix::spc {

namespace {
// llm_duration_seconds histogram upper bounds (seconds).
constexpr double kDurBounds[HypeMetrics::kDurBucketCount] = {
    0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0};
const char* kDurBoundStr[HypeMetrics::kDurBucketCount] = {
    "0.1", "0.25", "0.5", "1", "2", "5", "10"};
}  // namespace

const char* ToString(HypeMetrics::LlmCallStatus status) {
    switch (status) {
        case HypeMetrics::LlmCallStatus::kSuccess: return "success";
        case HypeMetrics::LlmCallStatus::kFailed:  return "failed";
    }
    return "success";
}

const char* ToString(HypeMetrics::MatchHitType hit_type) {
    switch (hit_type) {
        case HypeMetrics::MatchHitType::kChunk: return "chunk";
        case HypeMetrics::MatchHitType::kHype:  return "hype";
        case HypeMetrics::MatchHitType::kBoth:  return "both";
    }
    return "chunk";
}

HypeMetrics& HypeMetrics::Instance() {
    static HypeMetrics instance;
    return instance;
}

void HypeMetrics::RecordLlmCall(LlmCallStatus status) {
    llm_calls_[static_cast<int>(status)].fetch_add(1, std::memory_order_relaxed);
}
uint64_t HypeMetrics::LlmCallCount(LlmCallStatus status) const {
    return llm_calls_[static_cast<int>(status)].load(std::memory_order_relaxed);
}

void HypeMetrics::ObserveLlmDuration(const std::string& model, double seconds) {
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
uint64_t HypeMetrics::LlmDurationCount(const std::string& model) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = llm_duration_.find(model);
    return it == llm_duration_.end() ? 0 : it->second.count;
}
double HypeMetrics::LlmDurationSum(const std::string& model) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = llm_duration_.find(model);
    return it == llm_duration_.end() ? 0.0 : it->second.sum;
}

void HypeMetrics::AddQuestionsGenerated(uint64_t n) {
    questions_generated_.fetch_add(n, std::memory_order_relaxed);
}
uint64_t HypeMetrics::QuestionsGeneratedCount() const {
    return questions_generated_.load(std::memory_order_relaxed);
}

void HypeMetrics::RecordMatch(MatchHitType hit_type) {
    matches_[static_cast<int>(hit_type)].fetch_add(1, std::memory_order_relaxed);
}
uint64_t HypeMetrics::MatchCount(MatchHitType hit_type) const {
    return matches_[static_cast<int>(hit_type)].load(std::memory_order_relaxed);
}

std::string HypeMetrics::RenderOpenMetrics() const {
    std::ostringstream os;
    os << "# TYPE cortrix_hype_index_llm_calls_total counter\n";
    for (int i = 0; i < kLlmStatusCount; ++i) {
        os << "cortrix_hype_index_llm_calls_total{status=\""
           << ToString(static_cast<LlmCallStatus>(i)) << "\"} "
           << llm_calls_[i].load(std::memory_order_relaxed) << "\n";
    }

    os << "# TYPE cortrix_hype_index_questions_generated_total counter\n";
    os << "cortrix_hype_index_questions_generated_total "
       << questions_generated_.load(std::memory_order_relaxed) << "\n";

    os << "# TYPE cortrix_hype_index_match_total counter\n";
    for (int i = 0; i < kMatchHitTypeCount; ++i) {
        os << "cortrix_hype_index_match_total{hit_type=\""
           << ToString(static_cast<MatchHitType>(i)) << "\"} "
           << matches_[i].load(std::memory_order_relaxed) << "\n";
    }

    os << "# TYPE cortrix_hype_index_llm_duration_seconds histogram\n";
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [model, h] : llm_duration_) {
            uint64_t cum = 0;
            for (int i = 0; i < kDurBucketCount; ++i) {
                cum += h.buckets[i];
                os << "cortrix_hype_index_llm_duration_seconds_bucket{model=\""
                   << model << "\",le=\"" << kDurBoundStr[i] << "\"} " << cum << "\n";
            }
            cum += h.buckets[kDurBucketCount];
            os << "cortrix_hype_index_llm_duration_seconds_bucket{model=\"" << model
               << "\",le=\"+Inf\"} " << cum << "\n";
            os << "cortrix_hype_index_llm_duration_seconds_sum{model=\"" << model
               << "\"} " << h.sum << "\n";
            os << "cortrix_hype_index_llm_duration_seconds_count{model=\"" << model
               << "\"} " << h.count << "\n";
        }
    }
    return os.str();
}

void HypeMetrics::ResetForTest() {
    for (auto& a : llm_calls_) a.store(0, std::memory_order_relaxed);
    for (auto& a : matches_) a.store(0, std::memory_order_relaxed);
    questions_generated_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mu_);
    llm_duration_.clear();
}

}  // namespace cortrix::spc
