#pragma once
#include <string>

#include "cortrix/query/i_scatter_executor.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/reranker.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/retrieval/cross_ns_types.h"
#include "cortrix/retrieval/sparse_index_registry.h"

namespace cortrix {
class OnnxEmbedder;
}  // namespace cortrix

namespace cortrix::query {

/// LiveSingleUnitExecutor — the D3.5 IScatterExecutor that runs F04's per-NS
/// pipeline against the live MVP retrieval stack (Q2 wiring).
///
/// Why a purpose-built executor rather than the stock SingleUnitExecutor: F02's
/// reranker reverse-looks-up chunk_text from a ChunkStore that is bound at
/// construction, but Cortrix has one store PER namespace (the per-request
/// NamespaceFacade). A single global reranker therefore cannot serve every NS's
/// store. This executor solves that by keeping the reranker's stateless ONNX
/// scoring (ScoreBatch — thread-safe, store-independent) shared, while binding the
/// chunk-text lookup to the current NS's store per call. The fusion ordering
/// mirrors F02 OnnxReranker::Rerank exactly (RerankerScoreFusion 0.7/0.3), so the
/// single-NS result matches the frozen reranker contract.
///
/// Per ExecuteForNamespace(ns): Acquire facade → Vector+BM25 → RRF (block_id) →
/// resolve child_id/chunk_text/metadata from the NS store → rerank (shared
/// ScoreBatch + RerankerScoreFusion) when ctx.rerank, else RRF order → truncate to
/// top_k → NamespaceQueryResult. A missing NS / store fault is folded in-band as
/// CX_ERR_INDEX_CORRUPT (topic 2.4 partial success), never thrown.
class LiveSingleUnitExecutor : public IScatterExecutor {
public:
    /// @param pool      the F05 namespace pool (NOT owned).
    /// @param embedder  shared F03 OnnxEmbedder for the vector route (NOT owned).
    /// @param fusion    shared inner-RRF for vector+bm25 (NOT owned).
    /// @param reranker  shared F02 reranker; its ScoreBatch is used per NS. May be
    ///                 null only if every query sets rerank=false.
    /// @param sparse_registry  F40 per-NS SPLADE index owner (NOT owned). When non-
    ///                 null the executor runs chunk-level 5-path RRF (dense + sparse
    ///                 + FTS5; contextualized/hype fed empty until F35/F38 wire in);
    ///                 when null it runs the dense+FTS5 (2-route) RRF only. A NS with
    ///                 no indexed sparse vectors yields an empty sparse list → the
    ///                 fusion degrades to the other paths (F40 §7.2 L2 fallback).
    /// @param candidate_multiplier / max_candidates  over-fetch sizing (F02 §top_N).
    LiveSingleUnitExecutor(cortrix::resource::INamespacePool& pool,
                           cortrix::OnnxEmbedder& embedder,
                           RRFFusion& fusion,
                           reranker::IReranker* reranker,
                           cortrix::retrieval::SparseIndexRegistry* sparse_registry = nullptr,
                           int candidate_multiplier = 3,
                           int max_candidates = 50);

    retrieval::NamespaceQueryResult ExecuteForNamespace(
        const QueryContext& ctx,
        const std::string& namespace_id,
        float oversample = 1.0f) override;

    /// candidate_k = min(top_k × multiplier × ceil(oversample), max_candidates),
    /// clamped to >= top_k and >= 1 (mirrors SingleUnitExecutor::CandidateK).
    int CandidateK(int top_k, float oversample) const;

private:
    cortrix::resource::INamespacePool& pool_;
    cortrix::OnnxEmbedder& embedder_;
    // Retained for ctor/DI compatibility; the per-NS recall now uses the chunk-level
    // multi-path RRF (FuseFivePathRrf) over child_id keys rather than the block_id
    // RRFFusion::Merge, so this reference is currently unused.
    [[maybe_unused]] RRFFusion& fusion_;
    reranker::IReranker* reranker_;
    cortrix::retrieval::SparseIndexRegistry* sparse_registry_;
    int candidate_multiplier_;
    int max_candidates_;
};

}  // namespace cortrix::query
