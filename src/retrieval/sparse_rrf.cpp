#include "cortrix/retrieval/sparse_rrf.h"

#include <algorithm>
#include <unordered_map>

namespace cortrix::retrieval {

const char* ToString(RrfPath path) {
    switch (path) {
        case RrfPath::kDense:          return "dense";
        case RrfPath::kContextualized: return "contextualized";
        case RrfPath::kSparse:         return "sparse";
        case RrfPath::kFts5:           return "fts5";
        case RrfPath::kHypeQuestion:   return "hype_question";
    }
    return "dense";
}

namespace {
// Accumulate one path's RRF contributions into the score/paths maps. rank is the
// 0-based position; contribution = 1/(k + rank).
void AccumulatePath(const std::vector<SparseHit>& list, RrfPath path, int k,
                    std::unordered_map<ChildId, float>& score,
                    std::unordered_map<ChildId, std::vector<RrfPath>>& paths) {
    for (size_t rank = 0; rank < list.size(); ++rank) {
        const ChildId& id = list[rank].child_id;
        score[id] += 1.0f / static_cast<float>(k + static_cast<int>(rank));
        paths[id].push_back(path);
    }
}
}  // namespace

std::vector<RrfFusedHit> FuseFivePathRrf(const FivePathInput& input, int top_n,
                                         int k) {
    if (k <= 0) k = kRrfKDefault;  // guard against a 1/0 contribution

    std::unordered_map<ChildId, float> score;
    std::unordered_map<ChildId, std::vector<RrfPath>> paths;

    AccumulatePath(input.dense, RrfPath::kDense, k, score, paths);
    AccumulatePath(input.contextualized, RrfPath::kContextualized, k, score, paths);
    AccumulatePath(input.sparse, RrfPath::kSparse, k, score, paths);
    AccumulatePath(input.fts5, RrfPath::kFts5, k, score, paths);
    AccumulatePath(input.hype, RrfPath::kHypeQuestion, k, score, paths);

    std::vector<RrfFusedHit> out;
    out.reserve(score.size());
    for (auto& [id, s] : score) {
        RrfFusedHit hit;
        hit.child_id = id;
        hit.rrf_score = s;
        hit.contributing_paths = std::move(paths[id]);
        // keep contributing_paths in stable label order (enum order).
        std::sort(hit.contributing_paths.begin(), hit.contributing_paths.end());
        out.push_back(std::move(hit));
    }

    // Sort by rrf_score DESC; tie-break by child_id for determinism.
    auto cmp = [](const RrfFusedHit& a, const RrfFusedHit& b) {
        if (a.rrf_score != b.rrf_score) return a.rrf_score > b.rrf_score;
        return a.child_id < b.child_id;
    };
    if (top_n > 0 && static_cast<int>(out.size()) > top_n) {
        std::partial_sort(out.begin(), out.begin() + top_n, out.end(), cmp);
        out.resize(top_n);
    } else {
        std::sort(out.begin(), out.end(), cmp);
    }
    return out;
}

}  // namespace cortrix::retrieval
