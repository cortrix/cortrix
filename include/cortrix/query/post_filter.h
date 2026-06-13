#pragma once
#include <vector>
#include "cortrix/query/scored_block.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/query_response.h"
#include "cortrix/store/cortrix_store.h"

namespace cortrix {

class PostFilter {
public:
    /// @param store: F02 CortrixStore instance (for loading block details)
    explicit PostFilter(CortrixStore& store);

    /// Apply in-memory filtering on RRF results
    /// 1. Load block details from store (batch by block_id)
    /// 2. Filter by filter conditions
    /// 3. Return filtered ResultItem list truncated to top_k
    ///
    /// @param blocks: RRF-fused ScoredBlock list
    /// @param filter: filter conditions
    /// @param top_k: final truncation count
    /// @return filtered ResultItem list (truncated to top_k)
    std::vector<ResultItem> Apply(
        const std::vector<ScoredBlock>& blocks,
        const QueryFilter& filter,
        int top_k);

    /// Doc-ID pre-filter path: fetch all blocks for a specific document from
    /// the store (bypasses HNSW/BM25), apply exclude_types filter, return
    /// chunks in chunk_index order (up to top_k).
    /// Used when QueryFilter::doc_id is non-empty.
    std::vector<ResultItem> FetchByDocId(
        const std::string& doc_id,
        const QueryFilter& filter,
        int top_k);

private:
    /// Check if a single block matches filter conditions
    static bool MatchFilter(const CortrixBlock& block,
                            const CortrixDoc& doc,
                            const QueryFilter& filter);

    /// source_path glob matching
    /// Supports: trailing '*' (prefix), leading '*' (suffix), both (contains),
    ///           and single interior '*' (prefix + suffix). Exact match when no '*'.
    static bool GlobMatch(const std::string& pattern, const std::string& path);

    CortrixStore& store_;
};

}  // namespace cortrix
