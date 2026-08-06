#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/retrieval/cross_ns_types.h"  // cortrix::retrieval::ResultItem

namespace cortrix::query {

/// One failed namespace inside meta.namespaces_failed[].
/// Carries the full GEN-Agent error contract so an Agent can decide per-NS whether
/// to retry (timeout) or notify ops (permanent) — see §3.2 decision matrix.
struct NamespaceFailure {
    std::string namespace_id;                  ///< serialized as "namespace"
    std::string error_code;                    ///< CX_ERR_* (CX_ERR_NS_TIMEOUT / CX_ERR_INDEX_CORRUPT)
    bool retryable = false;                    ///< GEN-Agent #6
    std::string category;                      ///< GEN-Agent #4 enum string
    std::optional<int> retry_after_ms;         ///< GEN-Agent #6 (null when not retryable)
    std::string message;
    /// GEN-Agent #5 structured_data (cross_ns_error.h RequiredStructuredDataKeys).
    /// "namespace" is injected at serialization; codes like kIndexCorrupt also
    /// carry {index_state}. Serialized as meta.namespaces_failed[].structured_data.
    nlohmann::json structured_data = nlohmann::json::object();
};

/// One source of a deduplicated chunk (deduplicated_chunks[].namespaces[]).
struct DedupSourceNs {
    std::string namespace_id;   ///< serialized as "namespace"
    float score = 0.0f;
    float rerank_score = 0.0f;
};

/// A chunk that was found in >1 NS and collapsed to one primary (simplified).
struct DeduplicatedChunkInfo {
    std::string content_hash;                ///< "sha256:..."
    std::string primary_namespace;           ///< the NS whose copy was kept (highest final score)
    std::vector<DedupSourceNs> namespaces;   ///< brief multi-source array (NS + score), >= 2 entries
};

/// Cross-NS response meta — exactly the **8 A/C-class fields**
/// (rerank_enabled + dedup_applied B2 fields were deleted). All present on
/// every response (GEN-Agent #2 A-class "always included"); warnings is C-class (only populated
/// under its trigger).
struct CrossNsMeta {
    std::vector<std::string> namespaces_queried;       ///< A — input echo (request order)
    std::vector<std::string> namespaces_succeeded;     ///< A — partial-success semantics
    std::vector<NamespaceFailure> namespaces_failed;   ///< A — partial-failure + recovery
    float coverage_ratio = 1.0f;                       ///< A — succeeded / queried (0.0-1.0)
    int latency_ms = 0;                                ///< A — performance
    std::vector<DeduplicatedChunkInfo> deduplicated_chunks;  ///< A — multi-source merge detail
    int deduplicated_chunks_count = 0;                 ///< A — # chunks collapsed
    std::vector<nlohmann::json> warnings;              ///< C — only under trigger (e.g. rerank_disabled)

    nlohmann::json ToJson() const;
};

/// Cross-NS query response. `error` is set ONLY for the overall
/// scatter-timeout partial case (CX_ERR_SCATTER_TIMEOUT, still HTTP 200 with
/// partial results — topic 2.7 principle 3). Hard failures (auth / too-many / unauthorized)
/// are thrown as CrossNsException and never reach this struct.
struct CrossNsResponse {
    std::vector<retrieval::ResultItem> results;
    CrossNsMeta meta;
    std::optional<agent_friendly::AgentFriendlyError> error;  ///< CX_ERR_SCATTER_TIMEOUT only

    /// Serialize to the §2.5 JSON body: {results, meta[, error]}.
    nlohmann::json ToJson() const;
};

}  // namespace cortrix::query
