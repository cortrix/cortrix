#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/retrieval/cross_ns_types.h"  // cortrix::retrieval::ResultItem

namespace cortrix::query {

/// One failed namespace inside meta.namespaces_failed[] (F04 §2.5 / topic 2.4 + 2.7).
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

/// One source of a deduplicated chunk (F04 §2.5 deduplicated_chunks[].namespaces[]).
struct DedupSourceNs {
    std::string namespace_id;   ///< serialized as "namespace"
    float score = 0.0f;
    float rerank_score = 0.0f;
};

/// A chunk that was found in >1 NS and collapsed to one primary (F04 §3.3 B simplified).
struct DeduplicatedChunkInfo {
    std::string content_hash;                ///< "sha256:..."
    std::string primary_namespace;           ///< the NS whose copy was kept (highest final score)
    std::vector<DedupSourceNs> namespaces;   ///< brief multi-source array (NS + score), >= 2 entries
};

/// One meta.document_coverage[] entry: how much of ONE document this response
/// actually carried. Positioned as a server-side judgement + audit field, NOT as a
/// decision input the caller is expected to act on.
///
/// Counted in CHILD BLOCKS, not bytes, and that is deliberate: the per-document byte
/// size is not populated on the ingest path (documents.file_size is 0 on a document
/// that imported successfully), and a child block's absolute byte span can only be
/// derived from its parent row, which this response does not fetch unless parent
/// expansion was requested. A block count is available without any extra read.
struct DocumentCoverageInfo {
    std::string source_path;         ///< friendly document name (already surfaced on results)
    int returned_child_blocks = 0;   ///< distinct child blocks of this doc present in results
    int64_t total_child_blocks = 0;  ///< the document's total child-block count
    /// Whether total_child_blocks is a real count. A document with no child blocks
    /// legitimately counts 0, so 0 must NOT double as "unknown" — that would conflate
    /// an empty document with a count the store could not produce, and a caller
    /// reading "0 of 0" would take it for full coverage. The serializer emits null
    /// only when this is false.
    bool total_known = false;
};

/// F04 cross-NS response meta — exactly the **8 A/C-class fields** (F04 §2.5 / topic 3.5
/// v1.0.2; rerank_enabled + dedup_applied B2 fields were deleted). All present on
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
    /// A — how much of each returned document this response carried. One entry per
    /// DISTINCT document in `results`. Deliberately NOT named coverage_ratio: that
    /// name is taken, at this very level, by the namespace partial-success ratio
    /// above, and two meanings under one name would mislead exactly the caller this
    /// field is meant to inform.
    std::vector<DocumentCoverageInfo> document_coverage;
    /// doc_id → that document's total child-block count, carried up from the per-NS
    /// executors (the only layer with a store handle) so the denominator survives
    /// RAG-fusion variant merging and is still available when coverage is finalized.
    /// Internal accounting input; never serialized.
    std::map<std::string, int64_t> doc_child_block_totals;

    nlohmann::json ToJson() const;
};

/// Serialize explain.document_coverage[]. Lives next to the response types because
/// the route (not the response body) is what attaches it: it is a B-class diagnostic
/// under AGENT_FRIENDLY principle 2, so it belongs under `explain`, while `meta` stays
/// the fixed set of 8 A/C-class fields.
nlohmann::json DocumentCoverageArrayToJson(const std::vector<DocumentCoverageInfo>& coverage);

/// F04 cross-NS query response (F04 §2.5). `error` is set ONLY for the overall
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

/// Enforce the parent-expansion budget against the response as it will actually be
/// sent, and nothing earlier.
///
/// The per-namespace pass fetches parent text under its own copy of the limits, which
/// is not a limit on the response: with N namespaces each spending the byte budget, N
/// times the budget goes on the wire. The stages after the cross-NS gather — RAG-fusion
/// re-ordering, listwise rerank and its trim back to the requested top_k, CRAG's
/// ambiguous-verdict halving — also re-order and drop items, so a decision made before
/// them describes a different list than the caller receives.
///
/// Store-free and idempotent: the budget is applied by CLEARING parent text beyond the
/// limits. `expand_top_k <= 0` clears all of it; `max_bytes <= 0` means no byte cap.
void EnforceParentExpansionBudget(CrossNsResponse& resp, int expand_top_k, int max_bytes);

/// Recompute meta.document_coverage from the response's FINAL result set.
///
/// Must run after the last stage that mutates `results`: RAG-fusion re-ordering,
/// listwise rerank and its trim, and CRAG's halving all change the list, so a count
/// taken earlier can describe chunks that were since dropped or miss a document that
/// only a variant retrieved. Recomputed rather than adjusted, so calling it at the
/// gather and again in the route cannot drift.
///
/// Denominators come from meta.doc_child_block_totals; a document absent from that map
/// is reported with total_known = false rather than a fabricated zero.
void FinalizeDocumentCoverage(CrossNsResponse& resp);

}  // namespace cortrix::query
