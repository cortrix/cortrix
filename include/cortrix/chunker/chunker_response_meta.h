#pragma once
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/chunker/child_parent_dedup.h"
#include "cortrix/chunker/i_chunker.h"

// Agent-friendly response meta builders. Turn a
// ChunkerOutput's stats + the retrieval-time dedup groups into the GEN-Agent
// `meta` JSON the API/SDK/MCP boundary returns:
//   - index-time:  meta.stats (document-parse completeness, A-class) +
//                  meta.warnings[] (CX_WARN_CHUNK_FALLBACK when degraded)
//   - query-time:  meta.children_hits_per_parent[] (parent-child expansion hit detail, A-class)
//
// v1.0.1 revision (topic 6): rerank_enabled / parent_child_enabled / dedup_applied are
// NOT emitted (B2 — always-on / NS-known). Only the A-class fields above remain.
namespace cortrix::chunker {

/// Build the index-time `meta.stats` object from chunker stats.
nlohmann::json BuildStatsMeta(const ChunkerStats& stats);

/// Build the index-time `meta.warnings[]` array. Includes a
/// CX_WARN_CHUNK_FALLBACK entry when stats.fallback_to_flat; per-page parse
/// warnings (example) are appended by the parser/pipeline layer (their text
/// is not on ChunkerStats), so this returns just the chunker-owned warnings.
nlohmann::json BuildWarningsMeta(const ChunkerStats& stats, const std::string& doc_id,
                                 int parent_count, int threshold);

/// Build the query-time `meta.children_hits_per_parent[]` array from
/// deduped groups: [{parent_id, hits:[...], primary_child}, ...].
nlohmann::json BuildChildrenHitsMetaJson(const std::vector<ParentHitGroup>& groups);

}  // namespace cortrix::chunker
