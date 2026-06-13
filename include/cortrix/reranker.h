#pragma once
#include <string>
#include <vector>

#include "cortrix/reranker/reranker_config.h"
#include "cortrix/retrieval/types.h"   // ScoredResult / RankedChunk
#include "cortrix/store/chunk_store.h"  // ChunkStore (F02 §2.1-bis SoT)

namespace cortrix::reranker {

/// IReranker — the pluggable Cross-Encoder reranker abstraction (ARCH §4.4.1 /
/// F02 §2.1). V1 implementation = OnnxReranker (bge-reranker-v2-m3); V2+ may add
/// LlamaCppReranker / ApiReranker behind the same interface.
class IReranker {
public:
    virtual ~IReranker() = default;

    /// Score one query-passage pair (low-level score primitive).
    virtual float Score(const char* query, const char* passage) = 0;

    /// Batch-score a single query against many passages (4 workers in parallel;
    /// low-level score primitive).
    virtual std::vector<float> ScoreBatch(
        const char* query,
        const std::vector<const char*>& passages) = 0;

    /// Rerank — the full rerank link (F02 §2.1 / §4.2-bis; V5 ruling #4A).
    /// Input : F36 RRF-fused ScoredResult[] (child_id + RRF score) + query.
    /// Output: vector<RankedChunk> (field SoT = RETRIEVAL_TYPES_SPEC §1).
    /// Internal steps (§4.2-bis): ChunkStore.GetBatch(child_id) → ScoreBatch →
    /// RerankerScoreFusion::ComputeRerankRrfScore (F02-owned RRF fusion, sort-only)
    /// → sort by fused score → return.
    /// Callers: F04 ScatterGather, F37 CRAG Evaluator.
    virtual std::vector<retrieval::RankedChunk> Rerank(
        const std::vector<retrieval::ScoredResult>& candidates,
        const std::string& query) = 0;

    /// Reranker name (logging / debug).
    virtual const char* Name() const = 0;
};

/// Factory (V1 only constructs OnnxReranker). v1.0.3: Rerank needs a ChunkStore
/// to reverse-look-up chunk_text. Returns nullptr if construction fails (the
/// fail-fast CX_ERR_RERANKER_INIT_FAILED path is exercised by OnnxReranker::Init).
IReranker* CreateReranker(const RerankerConfig& config,
                          store::ChunkStore* chunk_store);

}  // namespace cortrix::reranker
