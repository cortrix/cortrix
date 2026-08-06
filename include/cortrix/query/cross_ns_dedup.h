#pragma once
#include <vector>

#include "cortrix/query/cross_ns_response.h"     // CrossNsMeta
#include "cortrix/retrieval/cross_ns_types.h"    // ResultItem

namespace cortrix::query {

/// DedupeByContentHash — simplified cross-NS deduplication.
///
/// When the same chunk (identical `content_hash`) is returned by more than one NS,
/// the copies collapse to a single result whose **primary = the highest final score
/// source**. The primary's full metadata / content / parent_content is kept
/// ("primary metadata"); the other sources are recorded — only their NS +
/// final score + rerank_score — as a brief multi-source entry in
/// `meta.deduplicated_chunks[]`, and
/// `meta.deduplicated_chunks_count` is set to the number of collapsed (removed)
/// copies. Singletons (a hash seen in exactly one NS) pass through untouched and are
/// NOT listed in deduplicated_chunks.
///
/// Order is preserved by **first appearance of each hash** in `items`, so a caller
/// that pre-sorts by final score (the gather order, ARCH) keeps the deduped
/// list in descending final score (the primary of each group is, by construction,
/// the first occurrence of its hash in a final-score-descending list). Items with
/// an empty content_hash are treated as distinct (never collapsed) — a missing hash
/// must not merge unrelated chunks.
///
/// 🚨 standalone: operates purely on in-memory ResultItems whose content_hash was
/// set by the gather layer (content-derived standalone; Block-header sourced once
/// that hook is wired). No storage / index access.
///
/// @param items  [in/out] candidate items; rewritten in place to the deduped set.
/// @param meta   [out] deduplicated_chunks[] + deduplicated_chunks_count populated.
/// @return the number of collapsed (removed) copies (== deduplicated_chunks_count;
///         also fed to the cortrix_scatter_dedup_collisions_total metric).
int DedupeByContentHash(std::vector<retrieval::ResultItem>& items, CrossNsMeta& meta);

}  // namespace cortrix::query
