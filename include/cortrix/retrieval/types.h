#pragma once
#include <map>
#include <string>
#include <vector>

#include "cortrix/common/score_signals.h"
#include "cortrix/id/types.h"  // ARCH §1.8.1 ID type SoT (cortrix::id::*)

namespace cortrix::retrieval {

/// Cortrix Retrieval-link type SoT (design/RETRIEVAL_TYPES_SPEC.md §1). F02 is the
/// first consumer (per F02 §2 it `#include "cortrix/retrieval/types.h"`), so this
/// header is introduced here.
///
/// ID types (ARCH §1.8.1): the canonical `cortrix::id` namespace is the single SoT
/// for ID type aliases. Per §1.8.1 line 1414 ("a Feature must not independently use ChildId =
/// std::string; ... it should reference cortrix::id::ChildId instead"), this header does NOT define a
/// local alias; it re-exports the canonical ones so `cortrix::retrieval::ChildId`
/// remains usable (and = std::string) for downstream code with zero churn.
using cortrix::id::ChildId;   ///< ULID, 26 chars — SoT cortrix::id (ARCH §1.8.1)
using cortrix::id::ParentId;  ///< doc-level parent block id — SoT cortrix::id

/// Raw retrieval result (minimal) — VectorSearcher / BM25Searcher / RRFFusion
/// output, F36 RRF feeds these into F02.Rerank as candidates.
struct ScoredResult {
    ChildId child_id;
    float   score = 0.0f;
};

/// Reranker output (carries full chunk content) — F02 output / F37 input.
/// Field SoT = RETRIEVAL_TYPES_SPEC §1.
struct RankedChunk {
    ChildId     child_id;
    std::string chunk_text;
    /// F34 §2.5 ParentChunkStore reverse-lookup. Filled at query time only when the
    /// request opts in (`include_parent`) and the hit is inside the expansion bound;
    /// empty otherwise. SPEC §2 v1.2: the expansion is bounded by request-scoped
    /// controls and defaults to off, because expanding a whole top_k of ~1024-token
    /// parents overflows the agent-side single-response budget and gets truncated.
    std::string parent_text;
    float       score = 0.0f;        ///< final ordering score after F02/F07 composition
    float       rerank_score = 0.0f; ///< F02 cross-encoder score
    ScoreSignals score_signals;      ///< optional F03/F07 query-time scoring signals
    std::map<std::string, std::string> metadata;
    /// Owning parent block ULID — the F34 §2.5 reverse-lookup key (SPEC §1 v1.2).
    /// Added because `ResultItem.parent_id` is a V5 ruling #6A primary identifier
    /// whose only producer (`ToResultItem(const RankedChunk&)`) had no source field,
    /// so it could only ever emit the default. Empty when the hit is not a child
    /// block (e.g. a doc-level pseudo-result).
    ///
    /// Deliberately LAST: SPEC §3.3 routes a field-order change through a
    /// cross-language ABI review, and appending keeps every existing positional
    /// aggregate initializer (`RankedChunk{child, text, parent_text, ...}`) valid.
    ParentId parent_id;
    /// What `chunk_text` actually holds (SPEC §1 v1.3). The doc-discovery path has a
    /// five-level fallback that can put a generated summary, a title, a filename or
    /// even an internal document id into this same field, so a caller previously had
    /// no way to tell a real passage from a bare identifier. One of:
    ///   "chunk"            — a real passage from the document
    ///   "document_summary" — a generated summary of the document, not its text
    ///   "document_label"   — only a label for the document (title / filename /
    ///                        internal id): this hit carries NO body evidence
    ///   "parent"           — a parent span; NOT produced in V1.0 (parents travel in
    ///                        the separate `parent_text` field), reserved
    /// Appended last for the same ABI reason as `parent_id`.
    std::string evidence_kind;
};

}  // namespace cortrix::retrieval
