#include "cortrix/spc/contextual_metrics.h"

#include <cmath>
#include <sstream>

namespace cortrix::spc {

const char* ToString(ContextualRetrievalMetrics::ChunkStatus status) {
    switch (status) {
        case ContextualRetrievalMetrics::ChunkStatus::kGenerated:    return "generated";
        case ContextualRetrievalMetrics::ChunkStatus::kFailed:       return "failed";
        case ContextualRetrievalMetrics::ChunkStatus::kSkippedNoLlm: return "skipped_no_llm";
    }
    return "generated";
}

const char* ToString(ContextualRetrievalMetrics::QueryPath path) {
    switch (path) {
        case ContextualRetrievalMetrics::QueryPath::kDense:          return "dense";
        case ContextualRetrievalMetrics::QueryPath::kContextualized: return "contextualized";
        case ContextualRetrievalMetrics::QueryPath::kFallbackDense:  return "fallback_dense";
    }
    return "dense";
}

ContextualRetrievalMetrics& ContextualRetrievalMetrics::Instance() {
    static ContextualRetrievalMetrics instance;
    return instance;
}

void ContextualRetrievalMetrics::RecordChunk(ChunkStatus status) {
    chunks_[static_cast<int>(status)].fetch_add(1, std::memory_order_relaxed);
}
uint64_t ContextualRetrievalMetrics::ChunkCount(ChunkStatus status) const {
    return chunks_[static_cast<int>(status)].load(std::memory_order_relaxed);
}

void ContextualRetrievalMetrics::AddLlmCostTokens(const std::string& model,
                                                  int64_t tokens) {
    std::lock_guard<std::mutex> lk(mu_);
    cost_tokens_by_model_[model] += tokens;
}
int64_t ContextualRetrievalMetrics::LlmCostTokens(const std::string& model) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cost_tokens_by_model_.find(model);
    return it == cost_tokens_by_model_.end() ? 0 : it->second;
}

void ContextualRetrievalMetrics::SetFallbackRatio(double ratio) {
    // Clamp to [0,1] then scale to an integer so the gauge stays lock-free.
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    fallback_ratio_scaled_.store(static_cast<int64_t>(std::llround(ratio * kRatioScale)),
                                 std::memory_order_relaxed);
}
double ContextualRetrievalMetrics::FallbackRatio() const {
    return static_cast<double>(fallback_ratio_scaled_.load(std::memory_order_relaxed)) /
           kRatioScale;
}

void ContextualRetrievalMetrics::RecordQueryPath(QueryPath path) {
    query_paths_[static_cast<int>(path)].fetch_add(1, std::memory_order_relaxed);
}
uint64_t ContextualRetrievalMetrics::QueryPathCount(QueryPath path) const {
    return query_paths_[static_cast<int>(path)].load(std::memory_order_relaxed);
}

std::string ContextualRetrievalMetrics::RenderOpenMetrics() const {
    std::ostringstream os;
    os << "# TYPE cortrix_contextual_retrieval_chunks_total counter\n";
    for (int i = 0; i < kChunkStatusCount; ++i) {
        os << "cortrix_contextual_retrieval_chunks_total{status=\""
           << ToString(static_cast<ChunkStatus>(i)) << "\"} "
           << chunks_[i].load(std::memory_order_relaxed) << "\n";
    }

    os << "# TYPE cortrix_contextual_retrieval_llm_cost_tokens counter\n";
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [model, tokens] : cost_tokens_by_model_) {
            os << "cortrix_contextual_retrieval_llm_cost_tokens{model=\"" << model
               << "\"} " << tokens << "\n";
        }
    }

    os << "# TYPE cortrix_contextual_retrieval_fallback_ratio gauge\n";
    os << "cortrix_contextual_retrieval_fallback_ratio " << FallbackRatio() << "\n";

    os << "# TYPE cortrix_contextual_retrieval_query_via_path_total counter\n";
    for (int i = 0; i < kQueryPathCount; ++i) {
        os << "cortrix_contextual_retrieval_query_via_path_total{path=\""
           << ToString(static_cast<QueryPath>(i)) << "\"} "
           << query_paths_[i].load(std::memory_order_relaxed) << "\n";
    }
    return os.str();
}

void ContextualRetrievalMetrics::ResetForTest() {
    for (auto& a : chunks_) a.store(0, std::memory_order_relaxed);
    for (auto& a : query_paths_) a.store(0, std::memory_order_relaxed);
    fallback_ratio_scaled_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mu_);
    cost_tokens_by_model_.clear();
}

}  // namespace cortrix::spc
