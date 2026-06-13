#include "cortrix/chunker/child_parent_dedup.h"

#include <algorithm>
#include <unordered_map>

namespace cortrix::chunker {

std::vector<ParentHitGroup> DedupChildrenToParents(
    const std::vector<ChildHit>& hits, int children_per_parent_for_rerank) {
    const int cap = std::max(1, children_per_parent_for_rerank);

    // Group by parent_id; flat-fallback hits (empty parent_id) get a synthetic key
    // (child_id) so each is its own singleton group and passes through unchanged.
    struct Accum {
        cortrix::id::ParentId parent_id;
        std::vector<ChildHit> hits;
    };
    std::unordered_map<std::string, Accum> by_key;
    std::vector<std::string> key_order;  // first-seen order, only for stable iteration
    for (const auto& h : hits) {
        const std::string key = h.parent_id.empty() ? ("\x01" + h.child_id) : h.parent_id;
        auto it = by_key.find(key);
        if (it == by_key.end()) {
            Accum a;
            a.parent_id = h.parent_id;  // empty for flat
            a.hits.push_back(h);
            by_key.emplace(key, std::move(a));
            key_order.push_back(key);
        } else {
            it->second.hits.push_back(h);
        }
    }

    std::vector<ParentHitGroup> groups;
    groups.reserve(by_key.size());
    for (const auto& key : key_order) {
        Accum& a = by_key[key];
        // Sort this parent's hits by score desc, tie-break child_id asc (determinism).
        std::sort(a.hits.begin(), a.hits.end(), [](const ChildHit& x, const ChildHit& y) {
            if (x.score != y.score) return x.score > y.score;
            return x.child_id < y.child_id;
        });

        ParentHitGroup g;
        g.parent_id = a.parent_id;
        g.primary_child_id = a.hits.front().child_id;
        g.top_score = a.hits.front().score;
        for (size_t i = 0; i < a.hits.size(); ++i) {
            g.all_hit_child_ids.push_back(a.hits[i].child_id);
            if (static_cast<int>(i) < cap) g.rerank_child_ids.push_back(a.hits[i].child_id);
        }
        groups.push_back(std::move(g));
    }

    // Best parent first; tie-break parent_id (primary_child for flat) for determinism.
    std::sort(groups.begin(), groups.end(), [](const ParentHitGroup& x, const ParentHitGroup& y) {
        if (x.top_score != y.top_score) return x.top_score > y.top_score;
        const std::string& xk = x.parent_id.empty() ? x.primary_child_id : x.parent_id;
        const std::string& yk = y.parent_id.empty() ? y.primary_child_id : y.parent_id;
        return xk < yk;
    });
    return groups;
}

std::vector<ChildrenHitsPerParent> BuildChildrenHitsMeta(
    const std::vector<ParentHitGroup>& groups) {
    std::vector<ChildrenHitsPerParent> meta;
    meta.reserve(groups.size());
    for (const auto& g : groups) {
        ChildrenHitsPerParent e;
        e.parent_id = g.parent_id;
        e.hits = g.all_hit_child_ids;
        e.primary_child = g.primary_child_id;
        meta.push_back(std::move(e));
    }
    return meta;
}

}  // namespace cortrix::chunker
