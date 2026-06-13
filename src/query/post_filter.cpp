#include "cortrix/query/post_filter.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/scored_block.h"

namespace cortrix {

PostFilter::PostFilter(CortrixStore& store)
    : store_(store) {}

std::vector<ResultItem> PostFilter::Apply(
    const std::vector<ScoredBlock>& blocks,
    const QueryFilter& filter,
    int top_k) {

    std::vector<ResultItem> results;
    results.reserve(std::min(static_cast<size_t>(top_k), blocks.size()));

    bool has_filter = !filter.block_types.empty() ||
                      !filter.exclude_types.empty() ||
                      !filter.source_path.empty() ||
                      !filter.created_after.empty();

    for (const auto& sb : blocks) {
        if (static_cast<int>(results.size()) >= top_k) break;

        // Load block details
        CortrixBlock block;
        int rc = store_.block_get(sb.block_id, block);
        if (rc != 0) {
            // Block may have been deleted during query; skip it
            continue;
        }

        // Load doc details
        CortrixDoc doc;
        rc = store_.doc_get(block.doc_id, doc);
        if (rc != 0) {
            // Doc may have been deleted; skip
            continue;
        }
        // [OPEN-2] A soft-deleted doc (Stage 1) is invisible to retrieval: its
        // blocks stay in the index (restore is free) but must not surface in
        // results until it is restored or hard-deleted by Stage 2.
        if (doc.status == DocStatus::kDeleted) {
            continue;
        }

        // Apply filter if present
        if (has_filter && !MatchFilter(block, doc, filter)) {
            continue;
        }

        // Build ResultItem
        ResultItem item;
        item.block_id = sb.block_id;
        item.doc_id = block.doc_id;
        item.score = sb.rrf_score;
        item.source_path = doc.source_path;
        item.block_type = BlockTypeToString(block.block_type);
        item.chunk_text = block.content_text;
        // item.metadata is the retrieval UNIT's metadata. Start from the doc-level
        // metadata, then overlay the BLOCK-level metadata so block-specific fields
        // win. Critical for MEM02 memory blocks: their identity fields
        // (user_id / memory_type / interaction_id) live in the block metadata, while
        // the synthetic __cortrix_memory__ doc carries none. The MEM05 scope filter
        // and the unified memory read pipeline both read item.metadata, so without the
        // block metadata every extracted fact is dropped as "no user_id".
        item.metadata = json::object();
        if (!doc.metadata_json.empty()) {
            try {
                item.metadata = json::parse(doc.metadata_json);
            } catch (...) {
                // Invalid doc metadata JSON; ignore, fall through to block metadata
            }
        }
        if (!block.metadata_json.empty()) {
            try {
                json bm = json::parse(block.metadata_json);
                if (bm.is_object()) {
                    for (auto it = bm.begin(); it != bm.end(); ++it) {
                        item.metadata[it.key()] = it.value();  // block overrides doc
                    }
                }
            } catch (...) {
                // Invalid block metadata JSON; keep doc-level metadata only
            }
        }
        item.related_blocks_count = sb.hit_count();
        item.hit_routes = sb.hit_routes;
        item.vector_score = sb.vector_score;
        item.bm25_score = sb.bm25_score;

        results.push_back(std::move(item));
    }

    return results;
}

bool PostFilter::MatchFilter(const CortrixBlock& block,
                              const CortrixDoc& doc,
                              const QueryFilter& filter) {
    // block_type include filter
    if (!filter.block_types.empty()) {
        std::string bt = BlockTypeToString(block.block_type);
        bool found = false;
        for (const auto& allowed : filter.block_types) {
            if (allowed == bt) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    // block_type exclude filter
    if (!filter.exclude_types.empty()) {
        std::string bt = BlockTypeToString(block.block_type);
        for (const auto& excluded : filter.exclude_types) {
            if (excluded == bt) {
                return false;
            }
        }
    }

    // source_path glob filter
    if (!filter.source_path.empty()) {
        if (!GlobMatch(filter.source_path, doc.source_path)) {
            return false;
        }
    }

    // created_after filter (ISO 8601 string comparison)
    if (!filter.created_after.empty()) {
        if (doc.created_at < filter.created_after) {
            return false;
        }
    }

    return true;
}

std::vector<ResultItem> PostFilter::FetchByDocId(
    const std::string& doc_id,
    const QueryFilter& filter,
    int top_k) {

    // Load all blocks for this doc from SQLite (in chunk_index order)
    std::vector<CortrixBlock> blocks;
    store_.block_get_by_doc(doc_id, blocks);

    // Load doc for source_path / metadata
    CortrixDoc doc;
    store_.doc_get(doc_id, doc);

    std::vector<ResultItem> results;
    // [OPEN-2] A soft-deleted doc is invisible to retrieval — return no rows.
    if (doc.status == DocStatus::kDeleted) {
        return results;
    }
    results.reserve(blocks.size());

    for (const auto& block : blocks) {
        if (static_cast<int>(results.size()) >= top_k) break;

        // Apply exclude_types filter
        if (!filter.exclude_types.empty()) {
            std::string bt = BlockTypeToString(block.block_type);
            bool excluded = false;
            for (const auto& ex : filter.exclude_types) {
                if (ex == bt) { excluded = true; break; }
            }
            if (excluded) continue;
        }

        // Skip empty blocks (no content to return)
        if (block.content_text.empty()) continue;

        ResultItem item;
        item.block_id   = block.block_id;
        item.doc_id     = block.doc_id;
        item.score      = 1.0f;   // doc-targeted: all chunks equally relevant
        item.source_path = doc.source_path;
        item.block_type = BlockTypeToString(block.block_type);
        item.chunk_text = block.content_text;
        item.metadata   = json::object();
        if (!doc.metadata_json.empty()) {
            try { item.metadata = json::parse(doc.metadata_json); }
            catch (...) {}
        }
        item.related_blocks_count = 1;
        item.hit_routes = kRouteDocId;
        item.vector_score = 0.0f;
        item.bm25_score   = 0.0f;

        results.push_back(std::move(item));
    }

    return results;
}

bool PostFilter::GlobMatch(const std::string& pattern, const std::string& path) {
    // Supports:
    //   - Exact match:     "/a/b.pdf" matches "/a/b.pdf"
    //   - Trailing '*':    "/contracts/*" matches "/contracts/2024/risk.pdf"
    //   - Leading '*':     "*.pdf" matches "/reports/2025/annual.pdf"
    //   - Both:            "*contracts*" matches "/my/contracts/old/x.pdf"
    //   - Mid-string '*':  "/data/*.pdf" matches "/data/report.pdf"
    //     (single '*' between fixed prefix and suffix)
    if (pattern.empty()) return true;

    bool starts_wild = (pattern.front() == '*');
    bool ends_wild   = (pattern.back() == '*');

    if (starts_wild && ends_wild) {
        // "*substr*" -- contains match
        if (pattern.size() <= 2) return true;  // "*" or "**" matches everything
        std::string infix = pattern.substr(1, pattern.size() - 2);
        return path.find(infix) != std::string::npos;
    }

    if (starts_wild) {
        // "*.pdf" -- suffix match
        std::string suffix = pattern.substr(1);
        if (suffix.size() > path.size()) return false;
        return path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    if (ends_wild) {
        // "/contracts/*" -- prefix match
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        return path.compare(0, prefix.size(), prefix) == 0;
    }

    // Check for a single interior wildcard: "prefix*suffix"
    auto star_pos = pattern.find('*');
    if (star_pos != std::string::npos) {
        std::string prefix = pattern.substr(0, star_pos);
        std::string suffix = pattern.substr(star_pos + 1);
        if (prefix.size() + suffix.size() > path.size()) return false;
        return path.compare(0, prefix.size(), prefix) == 0 &&
               path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // Exact match (no wildcards)
    return pattern == path;
}

}  // namespace cortrix
