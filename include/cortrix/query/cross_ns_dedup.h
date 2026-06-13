#pragma once
#include <vector>

#include "cortrix/query/cross_ns_response.h"     // CrossNsMeta
#include "cortrix/retrieval/cross_ns_types.h"    // ResultItem

namespace cortrix::query {

/// DedupeByContentHash — F04 §3.3 / §4.3 B simplified cross-NS deduplication.
///
/// When the same chunk (identical `content_hash`) is returned by more than one NS,
/// the copies collapse to a single result whose **primary = the highest-rerank_score
/// source** (§3.3.1). The primary's full metadata / content / parent_content is kept
/// (§3.3.3 "primary metadata"); the other sources are recorded — only their NS +
/// rerank_score — as a brief multi-source entry in `meta.deduplicated_chunks[]` (§2.5), and
/// `meta.deduplicated_chunks_count` is set to the number of collapsed (removed)
/// copies. Singletons (a hash seen in exactly one NS) pass through untouched and are
/// NOT listed in deduplicated_chunks.
///
/// Order is preserved by **first appearance of each hash** in `items`, so a caller
/// that pre-sorts by rerank_score (the gather order, ARCH §3.3) keeps the deduped
/// list in descending rerank_score (the primary of each group is, by construction,
/// the first occurrence of its hash in a rerank_score-descending list). Items with
/// an empty content_hash are treated as distinct (never collapsed) — a missing hash
/// must not merge unrelated chunks.
///
/// 🚨 D3 standalone: operates purely on in-memory ResultItems whose content_hash was
/// set by the gather layer (content-derived standalone; F09 Block-header sourced at
/// D3.5 — F09-4 hook). No storage / index access.
///
/// @param items  [in/out] candidate items; rewritten in place to the deduped set.
/// @param meta   [out] deduplicated_chunks[] + deduplicated_chunks_count populated.
/// @return the number of collapsed (removed) copies (== deduplicated_chunks_count;
///         also fed to the cortrix_scatter_dedup_collisions_total metric).
int DedupeByContentHash(std::vector<retrieval::ResultItem>& items, CrossNsMeta& meta);

}  // namespace cortrix::query
