#pragma once
#include <map>
#include <string>
#include <vector>

namespace cortrix::query {

struct SearchConfig {
    bool enable_vector = true;  ///< dense embedding/HNSW route
    bool enable_bm25 = true;    ///< FTS5/BM25 route
    bool enable_sparse = true;  ///< F40 sparse/SPLADE route when available
};

/// F04 cross-NS QueryRequest (F04 §2.4 — B' incompatible-with-MVP schema). The
/// ONLY supported request shape uses the `namespaces` **array** (topic 4.1). The MVP
/// single `namespace` field is deprecated → CX_ERR_DEPRECATED_FIELD at the HTTP
/// handler (S4.1 / W4, not in this batch).
///
/// This is `cortrix::query::QueryRequest`, distinct from the MVP single-NS
/// `cortrix::QueryRequest` (query/query_request.h, which has `namespace_name`).
/// ScatterGather consumes this one.
struct QueryRequest {
    std::string query;                          ///< query text (required)
    std::vector<std::string> namespaces;        ///< topic 4.1 — target NS array; ["*"] = wildcard
    int top_k = 10;                             ///< result count (default 10, range 1-100)
    bool rerank = true;                         ///< topic 4.3 — passed through to all NS; false → RRF fallback
    SearchConfig search_config;                 ///< optional route switches for ablation/diagnostics
    std::map<std::string, std::string> filter;  ///< topic 4.3 JSONB filter passed through (flattened)
};

}  // namespace cortrix::query
