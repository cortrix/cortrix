#include "cortrix/query/cross_ns_dedup.h"

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cortrix::query {

using retrieval::ResultItem;

int DedupeByContentHash(std::vector<ResultItem>& items, CrossNsMeta& meta) {
    // Group the indices of every item by its content_hash, preserving the order in
    // which each *distinct* hash first appears (so a final-score-sorted input keeps
    // its order through the dedup). Empty-hash items are never grouped — a missing
    // hash must not merge unrelated chunks — so each gets its own singleton bucket.
    std::unordered_map<std::string, std::size_t> hash_to_group;  // hash → groups[] idx
    std::vector<std::vector<std::size_t>> groups;                // group idx → item indices
    groups.reserve(items.size());

    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::string& h = items[i].content_hash;
        if (h.empty()) {
            groups.push_back({i});  // distinct singleton, not keyed
            continue;
        }
        auto it = hash_to_group.find(h);
        if (it == hash_to_group.end()) {
            hash_to_group.emplace(h, groups.size());
            groups.push_back({i});
        } else {
            groups[it->second].push_back(i);
        }
    }

    std::vector<ResultItem> deduped;
    deduped.reserve(groups.size());
    int collapsed = 0;

    for (const auto& group : groups) {
        if (group.size() == 1) {
            deduped.push_back(std::move(items[group[0]]));
            continue;
        }

        // Multi-NS hit on one content_hash. primary = highest final score source
        // (§3.3.1); ties resolve to the earliest occurrence (stable).
        std::size_t primary_idx = group[0];
        for (std::size_t k = 1; k < group.size(); ++k) {
            if (items[group[k]].score > items[primary_idx].score) {
                primary_idx = group[k];
            }
        }

        // brief multi-source entry (§2.5): content_hash + primary NS + scores per
        // source, in the items' encounter order so it is deterministic.
        DeduplicatedChunkInfo info;
        info.content_hash = items[primary_idx].content_hash;
        info.primary_namespace = items[primary_idx].namespace_id;
        info.namespaces.reserve(group.size());
        for (std::size_t idx : group) {
            DedupSourceNs src;
            src.namespace_id = items[idx].namespace_id;
            src.score = items[idx].score;
            src.rerank_score = items[idx].rerank_score;
            info.namespaces.push_back(std::move(src));
        }
        meta.deduplicated_chunks.push_back(std::move(info));

        // Keep the primary's full item (full metadata/content — §3.3.3); the other
        // copies are dropped. collapsed counts the removed copies.
        collapsed += static_cast<int>(group.size()) - 1;
        deduped.push_back(std::move(items[primary_idx]));
    }

    items = std::move(deduped);
    meta.deduplicated_chunks_count = collapsed;
    return collapsed;
}

}  // namespace cortrix::query
