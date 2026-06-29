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
    std::string parent_text;   ///< F34 §2.5 ParentChunkStore reverse-lookup (empty until F34 wires in)
    float       score = 0.0f;        ///< final ordering score after F02/F07 composition
    float       rerank_score = 0.0f; ///< F02 cross-encoder score
    ScoreSignals score_signals;      ///< optional F03/F07 query-time scoring signals
    std::map<std::string, std::string> metadata;
};

}  // namespace cortrix::retrieval
