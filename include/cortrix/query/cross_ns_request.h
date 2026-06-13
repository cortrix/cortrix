#pragma once
#include <map>
#include <string>
#include <vector>

namespace cortrix::query {

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
    std::map<std::string, std::string> filter;  ///< topic 4.3 JSONB filter passed through (flattened)
};

}  // namespace cortrix::query
