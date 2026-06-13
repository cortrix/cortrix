#pragma once
#include <string>
#include "cortrix/query/route_result.h"
#include "cortrix/store/cortrix_store.h"

namespace cortrix {

class BM25Searcher {
public:
    /// @param store: F02 CortrixStore instance (per-namespace)
    explicit BM25Searcher(CortrixStore& store);

    /// Execute BM25 full-text search
    /// Calls store.search_fulltext(query, top_k * 3, results)
    ///
    /// @param query_text: raw query text (FTS5 query)
    /// @param top_k: result count (oversampled to top_k * 3)
    /// @param timeout_us: route-level timeout (microseconds)
    /// @return RouteResult (route_name = "bm25")
    RouteResult Search(const std::string& query_text, int top_k, int64_t timeout_us);

private:
    /// FTS5 query sanitization: escape special chars
    static std::string SanitizeFtsQuery(const std::string& raw);

    CortrixStore& store_;
};

}  // namespace cortrix
