#include "cortrix/query/vector_searcher.h"
#include "cortrix/spc/onnx_embedder.h"
#include <chrono>

namespace cortrix {

VectorSearcher::VectorSearcher(store::IIndex& vec_index, OnnxEmbedder& embedder)
    : vec_index_(vec_index), embedder_(embedder) {}

RouteResult VectorSearcher::Search(const std::string& query_text, int top_k, int64_t timeout_us) {
    RouteResult result;
    result.route_name = "vector";

    auto start = std::chrono::steady_clock::now();

    // Step 1: Embed query text
    EmbeddingResult embed_result;
    Status embed_status = embedder_.Embed(query_text, &embed_result);
    if (!embed_status.ok()) {
        result.status = RouteStatus::kError;
        result.error_message = "embedding failed: " + embed_status.message();
        auto elapsed = std::chrono::steady_clock::now() - start;
        result.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        return result;
    }

    // Step 2: Check timeout after embedding
    auto elapsed_after_embed = std::chrono::steady_clock::now() - start;
    int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed_after_embed).count();
    if (elapsed_us > timeout_us) {
        result.status = RouteStatus::kTimeout;
        result.error_message = "timeout after embedding";
        result.latency_us = elapsed_us;
        return result;
    }

    // Step 3: Search vector index (oversample by 3x for post-filter). F01 IIndex
    // returns its hits directly (no rc; empty = no match), per iindex.h.
    int search_k = top_k * 3;
    auto hits = vec_index_.Search(embed_result.vector.data(), search_k);

    // Step 4: Convert (block_id, distance) hits -> RouteResultItem
    result.items.reserve(hits.size());
    for (const auto& [block_id, distance] : hits) {
        RouteResultItem item;
        item.block_id = block_id;
        item.doc_id.clear();  // IIndex hits don't carry doc_id; PostFilter resolves it
        item.chunk_index = 0;
        item.raw_score = 1.0f / (1.0f + distance);  // L2 distance -> similarity
        result.items.push_back(item);
    }

    result.status = RouteStatus::kSuccess;
    auto elapsed = std::chrono::steady_clock::now() - start;
    result.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    return result;
}

}  // namespace cortrix
