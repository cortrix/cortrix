#pragma once
#include <map>
#include <string>
#include <vector>

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

/// Parse a block's raw metadata_json and flatten its top-level fields into a
/// RankedChunk.metadata map (string→string). Each top-level key becomes a map
/// entry: string values are kept verbatim; non-string values are serialized with
/// json::dump() so the original scalar/array survives as text. This makes the
/// document's identity fields (e.g. beir_corpus_id) addressable as TOP-LEVEL
/// result-metadata keys — the cross-NS runner matches qrels on those keys, so
/// stuffing the whole blob under a single "metadata_json" key hid the identity
/// (the FiQA result-identity bug). Invalid / non-object JSON is ignored (no throw,
/// existing entries untouched), mirroring post_filter.cpp's try-catch tolerance.
void FlattenMetadataIntoMap(const std::string& metadata_json,
                            std::map<std::string, std::string>& out);

/// Classify one vector-route ANN hit for the
/// five-path RRF. A normal child block votes dense under its own child_id; a
/// hype_question block (block_type=16) votes hype under its SOURCE child
/// (metadata_json.source_child_id — the HyPE expansion; the question text never
/// impersonates a chunk). kDropped = missing identity (legacy row / bad metadata).
/// Contextual dual-vector hits are NOT classified here — they have no blocks row
/// (the caller resolves them through contextual_vec_labels).
enum class VectorHitPath { kDense, kHype, kDropped };
VectorHitPath ClassifyVectorHit(int block_type, const std::string& block_child_id,
                                const std::string& metadata_json,
                                std::string* out_child_id);

/// Hybrid RRF for granularity=auto/both. The fusion identity is the owning doc_id
/// (metadata.doc_id / source_doc_id), not child_id, so a doc-summary candidate and
/// its best chunk candidate reinforce each other instead of remaining disjoint. If a
/// document has both chunk and doc-summary evidence, the chunk is kept as the
/// user-facing representative and the doc evidence is preserved in metadata.
std::vector<retrieval::RankedChunk> FuseHybridChunksByDocId(
    std::vector<retrieval::RankedChunk> chunk_chunks,
    std::vector<retrieval::RankedChunk> doc_chunks,
    int top_k);

/// LiveSingleUnitExecutor — the live IScatterExecutor that runs the per-NS
/// pipeline against the live MVP retrieval stack (Q2 wiring).
///
/// Why a purpose-built executor rather than the stock SingleUnitExecutor: the reranker's
/// reranker reverse-looks-up chunk_text from a ChunkStore that is bound at
/// construction, but Cortrix has one store PER namespace (the per-request
/// NamespaceFacade). A single global reranker therefore cannot serve every NS's
/// store. This executor solves that by keeping the reranker's stateless ONNX
/// scoring (ScoreBatch — thread-safe, store-independent) shared, while binding the
/// chunk-text lookup to the current NS's store per call. The fusion ordering
/// mirrors OnnxReranker::Rerank exactly (RerankerScoreFusion 0.7/0.3), so the
/// single-NS result matches the frozen reranker contract.
///
/// Per ExecuteForNamespace(ns): Acquire facade → Vector+BM25 → RRF (block_id) →
/// resolve child_id/chunk_text/metadata from the NS store → rerank (shared
/// ScoreBatch + RerankerScoreFusion) when ctx.rerank, else RRF order → truncate to
/// top_k → NamespaceQueryResult. A missing NS / store fault is folded in-band as
/// CX_ERR_INDEX_CORRUPT (topic 2.4 partial success), never thrown.
class LiveSingleUnitExecutor : public IScatterExecutor {
public:
    /// @param pool      the namespace pool (NOT owned).
    /// @param embedder  shared OnnxEmbedder for the vector route (NOT owned).
    /// @param fusion    shared inner-RRF for vector+bm25 (NOT owned).
    /// @param reranker  shared reranker; its ScoreBatch is used per NS. May be
    ///                 null only if every query sets rerank=false.
    /// @param sparse_registry  per-NS SPLADE index owner (NOT owned). When non-
    ///                 null the executor runs chunk-level 5-path RRF (dense + sparse
    ///                 + FTS5; contextualized/hype fed empty until those stages wire in);
    ///                 when null it runs the dense+FTS5 (2-route) RRF only. A NS with
    ///                 no indexed sparse vectors yields an empty sparse list → the
    ///                 fusion degrades to the other paths (L2 fallback).
    /// @param candidate_multiplier / max_candidates  over-fetch sizing.
    /// @param doc_fts5_fallback_enabled  whether the doc path runs doc-level
    ///                 FTS5 fallback in addition to doc-summary HNSW.
    LiveSingleUnitExecutor(cortrix::resource::INamespacePool& pool,
                           cortrix::OnnxEmbedder& embedder,
                           RRFFusion& fusion,
                           reranker::IReranker* reranker,
                           cortrix::retrieval::SparseIndexRegistry* sparse_registry = nullptr,
                           int candidate_multiplier = 3,
                           int max_candidates = 50,
                           bool doc_fts5_fallback_enabled = true);

    /// Per-NS entry. Dispatches on ctx.granularity:
    ///   "auto" | "both" → ExecuteHybridRetrieval (chunk + doc-summary fallback);
    ///   "chunk" (and any unknown value) → ExecuteChunkRetrieval (explicit baseline);
    ///   "doc"  → ExecuteDocRetrieval (doc-discovery core: doc_summary HNSW +
    ///             optional doc-level FTS5 fallback);
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
    /// rerank → top_k). The granularity=chunk path calls it as the explicit baseline.
    retrieval::NamespaceQueryResult ExecuteChunkRetrieval(
        const QueryContext& ctx, const std::string& namespace_id, float oversample);

    /// granularity=doc (doc branch ≅ GET /documents/discover): run the shared
    /// Doc-discovery core (doc_summary HNSW + doc-level FTS5 fallback + doc_id
    /// RRF), surfaced as doc-level RankedChunks (child_id = doc_id). No rerank (doc
    /// summaries / doc pseudos are not chunk passages). A NS with no doc candidates
    /// returns an empty success result (not an error).
    retrieval::NamespaceQueryResult ExecuteDocRetrieval(
        const QueryContext& ctx, const std::string& namespace_id);

    /// granularity=auto/both (hybrid branch): run the chunk path AND the doc path,
    /// then fuse the two ranked lists with doc_id-keyed RRF before top_k. If a doc has
    /// both chunk and doc evidence, keep the chunk as the representative result while
    /// preserving doc-path evidence in metadata.
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
    bool doc_fts5_fallback_enabled_;
};

}  // namespace cortrix::query
