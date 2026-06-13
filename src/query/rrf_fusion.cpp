#include "cortrix/query/rrf_fusion.h"
#include <algorithm>
#include <unordered_map>

namespace cortrix {

RRFFusion::RRFFusion(int k) : k_(k) {}

std::vector<ScoredBlock> RRFFusion::Merge(const std::vector<RouteResult>& routes) const {
    std::unordered_map<int64_t, ScoredBlock> block_map;

    for (const auto& route : routes) {
        if (route.status != RouteStatus::kSuccess) continue;

        uint8_t route_flag = 0;
        if (route.route_name == "vector") route_flag = kRouteVector;
        else if (route.route_name == "bm25") route_flag = kRouteBM25;
        else if (route.route_name == "sql") route_flag = kRouteSQL;

        for (size_t i = 0; i < route.items.size(); ++i) {
            const auto& item = route.items[i];
            int rank = static_cast<int>(i) + 1;  // ranks start from 1
            float rrf_contribution = 1.0f / (k_ + rank);

            auto it = block_map.find(item.block_id);
            if (it != block_map.end()) {
                it->second.rrf_score += rrf_contribution;
                it->second.hit_routes |= route_flag;
                // Preserve raw scores from each route
                if (route_flag == kRouteVector) it->second.vector_score = item.raw_score;
                else if (route_flag == kRouteBM25) it->second.bm25_score = item.raw_score;
            } else {
                ScoredBlock sb;
                sb.block_id = item.block_id;
                sb.doc_id = item.doc_id;
                sb.chunk_index = item.chunk_index;
                sb.rrf_score = rrf_contribution;
                sb.hit_routes = route_flag;
                // Store raw score from this route
                if (route_flag == kRouteVector) sb.vector_score = item.raw_score;
                else if (route_flag == kRouteBM25) sb.bm25_score = item.raw_score;
                block_map[item.block_id] = sb;
            }
        }
    }

    std::vector<ScoredBlock> result;
    result.reserve(block_map.size());
    for (auto& [id, sb] : block_map) {
        result.push_back(std::move(sb));
    }

    // Sort by rrf_score descending (stable sort for determinism)
    std::stable_sort(result.begin(), result.end(),
        [](const ScoredBlock& a, const ScoredBlock& b) {
            return a.rrf_score > b.rrf_score;
        });

    return result;
}

std::vector<ScoredBlock> RRFFusion::Dedup(std::vector<ScoredBlock>& blocks) {
    std::unordered_map<int64_t, ScoredBlock> dedup_map;
    for (auto& sb : blocks) {
        auto it = dedup_map.find(sb.block_id);
        if (it != dedup_map.end()) {
            if (sb.rrf_score > it->second.rrf_score) {
                it->second.rrf_score = sb.rrf_score;
            }
            it->second.hit_routes |= sb.hit_routes;
            // Preserve non-zero raw scores from the incoming block
            if (sb.vector_score != 0.0f) it->second.vector_score = sb.vector_score;
            if (sb.bm25_score != 0.0f) it->second.bm25_score = sb.bm25_score;
        } else {
            dedup_map[sb.block_id] = sb;
        }
    }

    std::vector<ScoredBlock> result;
    result.reserve(dedup_map.size());
    for (auto& [id, sb] : dedup_map) {
        result.push_back(std::move(sb));
    }

    std::stable_sort(result.begin(), result.end(),
        [](const ScoredBlock& a, const ScoredBlock& b) {
            return a.rrf_score > b.rrf_score;
        });

    return result;
}

std::vector<ScoredBlock> RRFFusion::Truncate(const std::vector<ScoredBlock>& blocks, int top_k) {
    if (top_k <= 0) return {};
    size_t count = std::min(static_cast<size_t>(top_k), blocks.size());
    return std::vector<ScoredBlock>(blocks.begin(), blocks.begin() + count);
}

}  // namespace cortrix
