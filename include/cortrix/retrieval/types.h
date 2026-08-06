#pragma once
#include <map>
#include <string>
#include <vector>

#include "cortrix/common/score_signals.h"
#include "cortrix/id/types.h"  // ARCH ID type SoT (cortrix::id::*)

namespace cortrix::retrieval {

/// Cortrix retrieval-link type SoT. The reranker is the
/// first consumer (it `#include "cortrix/retrieval/types.h"`), so this
/// header is introduced here.
///
/// ID types (ARCH): the canonical `cortrix::id` namespace is the single SoT
/// for ID type aliases. Per line 1414 ("a Feature must not independently use ChildId =
/// std::string; ... it should reference cortrix::id::ChildId instead"), this header does NOT define a
/// local alias; it re-exports the canonical ones so `cortrix::retrieval::ChildId`
/// remains usable (and = std::string) for downstream code with zero churn.
using cortrix::id::ChildId;   ///< ULID, 26 chars — SoT cortrix::id (ARCH)
using cortrix::id::ParentId;  ///< doc-level parent block id — SoT cortrix::id

/// Raw retrieval result (minimal) — VectorSearcher / BM25Searcher / RRFFusion
/// output, RAG-Fusion RRF feeds these into Rerank as candidates.
struct ScoredResult {
    ChildId child_id;
    float   score = 0.0f;
};

/// Reranker output (carries full chunk content) — reranker output / CRAG input.
/// Field SoT = the retrieval-types spec
struct RankedChunk {
    ChildId     child_id;
    std::string chunk_text;
    std::string parent_text;   ///< ParentChunkStore reverse-lookup (empty until the chunker wires in)
    float       score = 0.0f;        ///< final ordering score after rerank/scoring composition
    float       rerank_score = 0.0f; ///< cross-encoder score
    ScoreSignals score_signals;      ///< optional query-time scoring signals
    std::map<std::string, std::string> metadata;
};

}  // namespace cortrix::retrieval
