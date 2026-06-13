#pragma once
#include <vector>
#include "cortrix/query/scored_block.h"
#include "cortrix/query/route_result.h"

namespace cortrix {

class RRFFusion {
public:
    /// @param k: RRF parameter, default 60
    explicit RRFFusion(int k = 60);

    /// RRF fusion: multiple RouteResult -> unified ScoredBlock list
    /// Algorithm: RRF_score(block) = sum(1 / (k + rank_i(block)))
    ///
    /// @param routes: search results from each route (only kSuccess routes)
    /// @return ScoredBlock list sorted by rrf_score descending, deduplicated
    std::vector<ScoredBlock> Merge(const std::vector<RouteResult>& routes) const;

    /// Deduplicate by block_id, keep highest rrf_score, merge hit_routes
    static std::vector<ScoredBlock> Dedup(std::vector<ScoredBlock>& blocks);

    /// Truncate to top_k
    static std::vector<ScoredBlock> Truncate(const std::vector<ScoredBlock>& blocks, int top_k);

private:
    int k_;
};

}  // namespace cortrix
