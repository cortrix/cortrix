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

    /// Per-NS entry. Dispatches on ctx.granularity (F41 §6.2 / §8.1):
    ///   "auto" | "chunk" (and any unknown value) → ExecuteChunkRetrieval (the existing
    ///       chunk-level pipeline, 100% unchanged — the default path is untouched);
    ///   "doc"  → ExecuteDocRetrieval (doc_summary embedding HNSW recall, §8.1 doc branch);
    ///   "both" → ExecuteHybridRetrieval (chunk + doc, §8.1 both branch).
    /// The cross-NS gather/dedupe (ScatterGather) is granularity-agnostic and untouched.
    retrieval::NamespaceQueryResult ExecuteForNamespace(
        const QueryContext& ctx,
        const std::string& namespace_id,
        float oversample = 1.0f) override;

    /// candidate_k = min(top_k × multiplier × ceil(oversample), max_candidates),
    /// clamped to >= top_k and >= 1 (mirrors SingleUnitExecutor::CandidateK).
    int CandidateK(int top_k, float oversample) const;

private:
    /// The existing chunk-level per-NS pipeline (Vector+BM25[+sparse] → multi-path RRF →
    /// rerank → top_k). This is the verbatim former ExecuteForNamespace body; the
    /// granularity=auto/chunk path calls it so the default behavior is byte-for-byte
    /// identical (F41 wiring iron rule — only doc/both diverge).
    retrieval::NamespaceQueryResult ExecuteChunkRetrieval(
        const QueryContext& ctx, const std::string& namespace_id, float oversample);

    /// granularity=doc (§8.1 doc branch ≅ GET /documents/discover): recall doc_summary
    /// blocks (block_type=17) from the per-NS P-HNSW via the shared
    /// doc_summary::RecallDocSummaryHnsw, surfaced as doc-level RankedChunks (child_id =
    /// doc_id, chunk_text = summary_text, metadata.via_path = "doc_summary"). No rerank
    /// (doc summaries are not chunk passages); HNSW order is kept. A NS with no
    /// doc_summary blocks returns an empty success result (not an error).
    retrieval::NamespaceQueryResult ExecuteDocRetrieval(
        const QueryContext& ctx, const std::string& namespace_id);

    /// granularity=both (§8.1 both branch): run the chunk path AND the doc path, then
    /// concatenate (chunk RankedChunks first, then doc-level), capped to top_k. The
    /// cross-NS gather still re-sorts by rerank_score, so this preserves both kinds in
    /// the candidate set; doc-level hits carry metadata.via_path = "doc_summary".
    retrieval::NamespaceQueryResult ExecuteHybridRetrieval(
        const QueryContext& ctx, const std::string& namespace_id, float oversample);

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
