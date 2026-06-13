#include "cortrix/spc/hype_fusion.h"

#include <algorithm>
#include <unordered_map>

namespace cortrix::spc {

namespace {
// Per-child accumulator during fusion.
struct Acc {
    float rrf_score = 0.0f;
    bool via_chunk = false;
    bool via_hype = false;
    std::string best_question;
    float best_hype_score = -1.0f;  // raw score of the best hype candidate
};
}  // namespace

std::vector<HypeFusedResult> FuseHypeRecall(
    const std::vector<HypeCandidate>& candidates,
    const std::map<ChildId, ParentId>& parent_of, int top_n, int rrf_k) {
    if (rrf_k <= 0) rrf_k = kHypeRrfK;  // guard 1/0

    // Step 1: split into the two rank paths. A path's rank order = the order the
    // candidates appear within that block_type (already score-sorted upstream).
    std::vector<const HypeCandidate*> chunk_path;
    std::vector<const HypeCandidate*> hype_path;
    for (const auto& c : candidates) {
        if (c.block_type == kBlockHypeQuestion) {
            hype_path.push_back(&c);
        } else {
            chunk_path.push_back(&c);  // any non-hype block_type = chunk path
        }
    }

    std::unordered_map<ChildId, Acc> acc;

    // Step 2a: chunk path RRF (keyed by the candidate's own child_id).
    for (size_t rank = 0; rank < chunk_path.size(); ++rank) {
        Acc& a = acc[chunk_path[rank]->child_id];
        a.rrf_score += 1.0f / static_cast<float>(rrf_k + static_cast<int>(rank));
        a.via_chunk = true;
    }
    // Step 2b: hype path RRF (keyed by source_child_id — the chunk it points to).
    for (size_t rank = 0; rank < hype_path.size(); ++rank) {
        const HypeCandidate* h = hype_path[rank];
        const ChildId& target = h->source_child_id;
        Acc& a = acc[target];
        a.rrf_score += 1.0f / static_cast<float>(rrf_k + static_cast<int>(rank));
        a.via_hype = true;
        if (h->score > a.best_hype_score) {  // keep the best matched question
            a.best_hype_score = h->score;
            a.best_question = h->question_text;
        }
    }

    // Materialize fused results + resolve parent_id.
    std::vector<HypeFusedResult> fused;
    fused.reserve(acc.size());
    for (auto& [child_id, a] : acc) {
        HypeFusedResult r;
        r.child_id = child_id;
        auto it = parent_of.find(child_id);
        r.parent_id = (it != parent_of.end()) ? it->second : ParentId{};
        r.rrf_score = a.rrf_score;
        r.via_hype = a.via_hype;
        if (a.via_hype) {
            r.hype_question_matched = a.best_question;
            r.hype_match_score = a.best_hype_score;
        }
        fused.push_back(std::move(r));
    }

    // Step 3: by-parent dedup — among children sharing a non-empty parent_id, keep
    // the highest rrf_score. Children with empty parent_id are each their own group.
    auto better = [](const HypeFusedResult& a, const HypeFusedResult& b) {
        if (a.rrf_score != b.rrf_score) return a.rrf_score > b.rrf_score;
        return a.child_id < b.child_id;  // deterministic tie-break
    };
    std::unordered_map<ParentId, size_t> best_by_parent;  // parent_id → index in kept
    std::vector<HypeFusedResult> kept;
    for (auto& r : fused) {
        if (r.parent_id.empty()) {
            kept.push_back(r);  // own group
            continue;
        }
        auto it = best_by_parent.find(r.parent_id);
        if (it == best_by_parent.end()) {
            best_by_parent[r.parent_id] = kept.size();
            kept.push_back(r);
        } else if (better(r, kept[it->second])) {
            kept[it->second] = r;  // replace with the better child of this parent
        }
    }

    // Step 4: sort DESC + truncate.
    if (top_n > 0 && static_cast<int>(kept.size()) > top_n) {
        std::partial_sort(kept.begin(), kept.begin() + top_n, kept.end(), better);
        kept.resize(top_n);
    } else {
        std::sort(kept.begin(), kept.end(), better);
    }
    return kept;
}

}  // namespace cortrix::spc
