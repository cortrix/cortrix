#include <cstdint>
#include "cortrix/query/live_single_unit_executor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

#include "cortrix/common/types.h"  // json alias (metadata_json flatten)
#include "cortrix/common/json_depth.h"  // metadata depth guard (DoS: deep-JSON dump)
#include "cortrix/doc_summary/discover_handler.h"  // RecallDocSummaryHnsw (granularity=doc/both)
#include "cortrix/doc_summary/doc_summary_types.h"  // DocDiscoveryHit
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/reranker/score_fusion.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/retrieval/sparse_codec.h"
#include "cortrix/retrieval/sparse_retriever.h"
#include "cortrix/retrieval/sparse_rrf.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/store/sqlite_chunk_store.h"

#include <unordered_map>

namespace cortrix::query {

using retrieval::NamespaceQueryResult;
using retrieval::RankedChunk;

namespace {

// Route-level deadline for the live two-route fan-out (the F04 per-NS timeout is
// enforced one level up by the ScatterGather join).
constexpr int64_t kRouteTimeoutUs = 5'000'000;  // 5s
constexpr int kHybridRrfK = 60;

struct HybridCandidate {
    RankedChunk chunk;
    std::string doc_id;
    float fused_score = 0.0f;
    float best_contribution = -1.0f;
    float best_chunk_contribution = -1.0f;
    float best_doc_contribution = -1.0f;
    float best_chunk_score = 0.0f;
    float best_doc_score = 0.0f;
    float best_chunk_rerank_score = 0.0f;
    float best_doc_rerank_score = 0.0f;
    bool has_chunk = false;
    bool has_doc = false;
    std::vector<std::string> paths;
    std::map<std::string, std::string> doc_evidence_metadata;
};

std::string MetadataValue(const RankedChunk& rc, const std::string& key) {
    auto it = rc.metadata.find(key);
    return it == rc.metadata.end() ? std::string() : it->second;
}

std::string HybridDocId(const RankedChunk& rc) {
    std::string doc_id = MetadataValue(rc, "doc_id");
    if (!doc_id.empty()) return doc_id;
    doc_id = MetadataValue(rc, "source_doc_id");
    if (!doc_id.empty()) return doc_id;
    doc_id = MetadataValue(rc, "hybrid_doc_id");
    if (!doc_id.empty()) return doc_id;
    // Compatibility fallback: a doc-path RankedChunk uses child_id=doc_id, while a
    // legacy chunk-path result may have no doc metadata. Falling back preserves the
    // candidate, but production chunk results now set doc_id explicitly below.
    return rc.child_id;
}

std::string HybridPath(const RankedChunk& rc, const std::string& fallback) {
    if (fallback == "chunk") return "chunk";
    const std::string via = MetadataValue(rc, "via_path");
    if (!via.empty()) return via;
    return fallback;
}

bool IsDocEvidencePath(const std::string& path) {
    return path != "chunk";
}

bool ContainsPath(const std::vector<std::string>& paths, const std::string& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

void AppendUniquePath(std::vector<std::string>& paths, const std::string& path) {
    if (!ContainsPath(paths, path)) paths.push_back(path);
}

std::string JoinPaths(const std::vector<std::string>& paths) {
    std::string out;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) out += ",";
        out += paths[i];
    }
    return out;
}

bool ShouldOverlayDocEvidence(const std::string& key) {
    return key == "doc_discovery_match_score" ||
           key == "doc_summary_match_score" ||
           key == "doc_fts5_match_score" ||
           key == "one_liner" ||
           key == "doc_title" ||
           key == "filename" ||
           key == "doc_discovery_via_path";
}

void CaptureDocEvidenceMetadata(HybridCandidate& cand, const RankedChunk& rc) {
    for (const auto& kv : rc.metadata) {
        if (kv.first == "via_path") continue;
        auto it = cand.doc_evidence_metadata.find(kv.first);
        if (it == cand.doc_evidence_metadata.end() || ShouldOverlayDocEvidence(kv.first)) {
            cand.doc_evidence_metadata[kv.first] = kv.second;
        }
    }
}

void ApplyDocEvidenceMetadata(HybridCandidate& cand) {
    for (const auto& kv : cand.doc_evidence_metadata) {
        auto it = cand.chunk.metadata.find(kv.first);
        if (it == cand.chunk.metadata.end() || ShouldOverlayDocEvidence(kv.first)) {
            cand.chunk.metadata[kv.first] = kv.second;
        }
    }
}

}  // namespace

std::vector<RankedChunk> FuseHybridChunksByDocId(std::vector<RankedChunk> chunk_chunks,
                                                 std::vector<RankedChunk> doc_chunks,
                                                 int top_k) {
    std::vector<HybridCandidate> candidates;
    candidates.reserve(chunk_chunks.size() + doc_chunks.size());
    std::unordered_map<std::string, std::size_t> by_doc;

    auto add_path = [&](std::vector<RankedChunk>& source, const std::string& path) {
        for (std::size_t rank = 0; rank < source.size(); ++rank) {
            RankedChunk rc = std::move(source[rank]);
            const std::string doc_id = HybridDocId(rc);
            if (doc_id.empty()) continue;

            const std::string source_path = HybridPath(rc, path);
            const bool is_doc_path = IsDocEvidencePath(source_path);
            const float source_score = rc.score;
            const float source_rerank_score = rc.rerank_score;
            const float contribution =
                1.0f / (static_cast<float>(kHybridRrfK) + static_cast<float>(rank));

            if (!is_doc_path || rc.metadata.find("via_path") == rc.metadata.end()) {
                rc.metadata["via_path"] = source_path;
            }
            if (rc.metadata.find("doc_id") == rc.metadata.end()) rc.metadata["doc_id"] = doc_id;
            if (rc.metadata.find("source_doc_id") == rc.metadata.end()) {
                rc.metadata["source_doc_id"] = doc_id;
            }
            rc.metadata["hybrid_doc_id"] = doc_id;
            rc.metadata["hybrid_source_path"] = source_path;
            rc.metadata["hybrid_source_score"] = std::to_string(source_score);
            rc.metadata["hybrid_source_rerank_score"] = std::to_string(source_rerank_score);

            auto it = by_doc.find(doc_id);
            if (it == by_doc.end()) {
                HybridCandidate cand;
                cand.doc_id = doc_id;
                cand.chunk = std::move(rc);
                cand.fused_score = contribution;
                cand.best_contribution = contribution;
                AppendUniquePath(cand.paths, source_path);
                if (is_doc_path) {
                    cand.has_doc = true;
                    cand.best_doc_contribution = contribution;
                    cand.best_doc_score = source_score;
                    cand.best_doc_rerank_score = source_rerank_score;
                    CaptureDocEvidenceMetadata(cand, cand.chunk);
                } else {
                    cand.has_chunk = true;
                    cand.best_chunk_contribution = contribution;
                    cand.best_chunk_score = source_score;
                    cand.best_chunk_rerank_score = source_rerank_score;
                }
                by_doc.emplace(cand.doc_id, candidates.size());
                candidates.push_back(std::move(cand));
                continue;
            }

            HybridCandidate& cand = candidates[it->second];
            cand.fused_score += contribution;
            AppendUniquePath(cand.paths, source_path);

            if (is_doc_path) {
                cand.has_doc = true;
                CaptureDocEvidenceMetadata(cand, rc);
                if (contribution > cand.best_doc_contribution) {
                    cand.best_doc_contribution = contribution;
                    cand.best_doc_score = source_score;
                    cand.best_doc_rerank_score = source_rerank_score;
                }
                // Keep a doc pseudo-result only when no chunk representative exists.
                if (!cand.has_chunk && contribution > cand.best_contribution) {
                    rc.metadata["hybrid_source_path"] = source_path;
                    cand.chunk = std::move(rc);
                    cand.best_contribution = contribution;
                }
                continue;
            }

            cand.has_chunk = true;
            if (contribution > cand.best_chunk_contribution) {
                cand.best_chunk_contribution = contribution;
                cand.best_chunk_score = source_score;
                cand.best_chunk_rerank_score = source_rerank_score;
            }
            // User-facing hybrid result should be the best chunk if one exists; doc
            // evidence remains attached via metadata. This avoids returning an LLM
            // pseudo-document when an actual passage for the same doc is available.
            if (contribution > cand.best_contribution ||
                IsDocEvidencePath(MetadataValue(cand.chunk, "via_path"))) {
                rc.metadata["hybrid_source_path"] = source_path;
                cand.chunk = std::move(rc);
                cand.best_contribution = contribution;
                ApplyDocEvidenceMetadata(cand);
            }
        }
    };

    add_path(chunk_chunks, "chunk");
    add_path(doc_chunks, "doc");

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const HybridCandidate& a, const HybridCandidate& b) {
                         return a.fused_score > b.fused_score;
                     });

    const int k = top_k < 1 ? 1 : top_k;
    std::vector<RankedChunk> fused;
    fused.reserve(std::min<std::size_t>(candidates.size(), static_cast<std::size_t>(k)));
    for (HybridCandidate& cand : candidates) {
        ApplyDocEvidenceMetadata(cand);
        cand.chunk.score = cand.fused_score;
        cand.chunk.metadata["doc_id"] = cand.doc_id;
        cand.chunk.metadata["source_doc_id"] = cand.doc_id;
        cand.chunk.metadata["hybrid_doc_id"] = cand.doc_id;
        cand.chunk.metadata["hybrid_rrf_score"] = std::to_string(cand.fused_score);
        cand.chunk.metadata["hybrid_paths"] = JoinPaths(cand.paths);
        cand.chunk.metadata["hybrid_has_chunk"] = cand.has_chunk ? "true" : "false";
        cand.chunk.metadata["hybrid_has_doc"] = cand.has_doc ? "true" : "false";
        if (cand.has_chunk) {
            cand.chunk.metadata["hybrid_chunk_score"] = std::to_string(cand.best_chunk_score);
            cand.chunk.metadata["hybrid_chunk_rerank_score"] =
                std::to_string(cand.best_chunk_rerank_score);
        }
        if (cand.has_doc) {
            cand.chunk.metadata["hybrid_doc_score"] = std::to_string(cand.best_doc_score);
            cand.chunk.metadata["hybrid_doc_rerank_score"] =
                std::to_string(cand.best_doc_rerank_score);
        }
        if (cand.has_chunk && cand.has_doc) {
            cand.chunk.metadata["via_path"] = "hybrid";
        } else if (cand.has_chunk) {
            cand.chunk.metadata["via_path"] = "chunk";
        } else if (!cand.paths.empty()) {
            cand.chunk.metadata["via_path"] = cand.paths.front();
        }
        fused.push_back(std::move(cand.chunk));
        if (static_cast<int>(fused.size()) >= k) break;
    }
    return fused;
}

void FlattenMetadataIntoMap(const std::string& metadata_json,
                            std::map<std::string, std::string>& out) {
    if (metadata_json.empty()) return;
    try {
        json parsed = json::parse(metadata_json);
        if (!parsed.is_object()) return;  // only object-shaped metadata flattens
        // Defense-in-depth: ingest now rejects over-deep metadata (json_depth.h), but
        // data stored before that guard -- or via any path that bypassed it -- could
        // still be deep enough to overflow the stack inside the it.value().dump()
        // below (a stack overflow is a SIGSEGV the catch cannot stop). Skip flattening
        // such a value rather than crash the query worker.
        if (JsonExceedsMaxDepth(parsed)) return;
        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
            // Keep string values verbatim (so beir_corpus_id stays its raw id, not a
            // quoted "\"id\""); serialize any non-string scalar/array/object via dump()
            // so it survives as text in the string→string result map.
            out[it.key()] = it.value().is_string()
                                ? it.value().get<std::string>()
                                : it.value().dump();
        }
    } catch (...) {
        // Invalid metadata JSON: ignore, leave the map as-is (post_filter.cpp parity).
    }
}

LiveSingleUnitExecutor::LiveSingleUnitExecutor(cortrix::resource::INamespacePool& pool,
                                               cortrix::OnnxEmbedder& embedder,
                                               RRFFusion& fusion,
                                               reranker::IReranker* reranker,
                                               cortrix::retrieval::SparseIndexRegistry* sparse_registry,
                                               int candidate_multiplier,
                                               int max_candidates,
                                               bool doc_fts5_fallback_enabled)
    : pool_(pool),
      embedder_(embedder),
      fusion_(fusion),
      reranker_(reranker),
      sparse_registry_(sparse_registry),
      candidate_multiplier_(candidate_multiplier < 1 ? 1 : candidate_multiplier),
      max_candidates_(max_candidates < 1 ? 1 : max_candidates),
      doc_fts5_fallback_enabled_(doc_fts5_fallback_enabled) {}

int LiveSingleUnitExecutor::CandidateK(int top_k, float oversample) const {
    int k = top_k < 1 ? 1 : top_k;
    int os = oversample > 1.0f ? static_cast<int>(std::ceil(oversample)) : 1;
    long long cand = static_cast<long long>(k) * candidate_multiplier_ * os;
    if (cand > max_candidates_) cand = max_candidates_;
    if (cand < k) cand = k;
    return static_cast<int>(cand);
}

NamespaceQueryResult LiveSingleUnitExecutor::ExecuteForNamespace(
    const QueryContext& ctx, const std::string& namespace_id, float oversample) {
    if (ctx.granularity == "doc") {
        return ExecuteDocRetrieval(ctx, namespace_id);
    }
    if (ctx.granularity == "auto" || ctx.granularity == "both") {
        return ExecuteHybridRetrieval(ctx, namespace_id, oversample);
    }
    return ExecuteChunkRetrieval(ctx, namespace_id, oversample);
}

NamespaceQueryResult LiveSingleUnitExecutor::ExecuteChunkRetrieval(
    const QueryContext& ctx, const std::string& namespace_id, float oversample) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    NamespaceQueryResult out;
    out.namespace_id = namespace_id;

    auto finish_latency = [&]() {
        out.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0)
                .count());
    };

    // A reranker is required unless the request opts out (RRF fallback) — same
    // contract as SingleUnitExecutor (a rerank=true query with no reranker is a
    // per-NS misconfiguration → CX_ERR_INDEX_CORRUPT).
    if (ctx.rerank && reranker_ == nullptr) {
        const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
        out.error_code = info.cx_code;
        out.error_category = agent_friendly::ToString(info.category);
        out.retryable = info.retryable;
        out.structured_data = {{"index_state", "reranker_unconfigured"}};  // GEN-Agent #5
        finish_latency();
        return out;
    }

    try {
        // 1. Acquire the NS façade (F05 Pool.Acquire, RAII-released at scope exit).
        cortrix::resource::NamespaceFacade facade(pool_, namespace_id);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
            out.error_code = info.cx_code;
            out.error_category = agent_friendly::ToString(info.category);
            out.retryable = info.retryable;
            // GEN-Agent #5: kIndexCorrupt requires {namespace, index_state}.
            // "unavailable" is deliberately non-committal — per cross_ns_error.h
            // anti-enumeration (§2.6/§5), a per-NS failure must NOT leak whether
            // the NS is absent vs internally broken, so we do not surface
            // "not_found" here.
            out.structured_data = {{"index_state", "unavailable"}};
            finish_latency();
            return out;
        }

        const int candidate_k = CandidateK(ctx.top_k, oversample);
        CortrixStore& store = facade.store();

        // 2. Run the enabled per-NS recall routes. By default vector + BM25 are on
        //    and F40 sparse joins when a sparse registry is wired; search_config can
        //    disable individual routes for diagnostic ablations such as dense-only.
        //    The retrieval-link boundary keys on child_id (ULID), so each route's
        //    block_id hits are mapped to child_id by reading the per-NS store; a
        //    legacy non-child row (empty child_id) is dropped. Blocks are cached here
        //    so the final RankedChunk assembly reuses them (one block_get per
        //    distinct child).
        RouteResult vector_result;
        vector_result.route_name = "vector";
        vector_result.status = RouteStatus::kSkipped;
        if (ctx.enable_vector) {
            VectorSearcher vec_searcher(facade.vec_index(), embedder_);
            vector_result = vec_searcher.Search(ctx.query, candidate_k, kRouteTimeoutUs);
        }

        RouteResult bm25_result;
        bm25_result.route_name = "bm25";
        bm25_result.status = RouteStatus::kSkipped;
        if (ctx.enable_bm25) {
            BM25Searcher bm25_searcher(facade.store());
            bm25_result = bm25_searcher.Search(ctx.query, candidate_k, kRouteTimeoutUs);
        }

        // child_id → resolved chunk (text + optional metadata) cache, built as the
        // routes are converted. Dense/FTS5 resolve the full block (metadata_json);
        // a sparse-only child resolves text via the ChunkStore reverse-lookup.
        struct ChunkRow {
            std::string content_text;
            std::string metadata_json;
            std::string doc_id;  // owning document, for doc-level metadata round-trip
            ScoreSignals score_signals;
        };
        std::unordered_map<std::string, ChunkRow> by_child;
        // doc_id → owning-document fields cache. The friendly source_path (the original
        // filename) and any caller-supplied metadata live on the doc ROW, not the content
        // block (the block only carries the internal materialize name). Both are surfaced
        // into result metadata below so query results / chat citations show the real file
        // name instead of an internal id. One doc_get per distinct doc in the result set.
        struct DocFields {
            std::string metadata_json;
            std::string source_path;
        };
        std::unordered_map<std::string, DocFields> doc_meta_cache;
        auto doc_fields = [&](const std::string& doc_id) -> const DocFields& {
            static const DocFields kEmpty;
            if (doc_id.empty()) return kEmpty;
            auto it = doc_meta_cache.find(doc_id);
            if (it != doc_meta_cache.end()) return it->second;
            CortrixDoc doc;
            DocFields df;
            if (store.doc_get(doc_id, doc) == 0) {
                df.metadata_json = doc.metadata_json;
                df.source_path = doc.source_path;
            }
            return doc_meta_cache.emplace(doc_id, std::move(df)).first->second;
        };

        // Convert a block_id-keyed RouteResult into a child_id-keyed ranked list
        // (RrfPath input), caching each resolved block. raw_score order is preserved
        // (the searchers already sort), so RRF rank = list position.
        auto to_child_hits = [&](const RouteResult& rr) {
            std::vector<retrieval::SparseHit> hits;
            if (!rr.ok()) return hits;
            hits.reserve(rr.items.size());
            for (const auto& it : rr.items) {
                CortrixBlock block;
                if (store.block_get(it.block_id, block) != 0) continue;
                if (block.child_id.empty()) continue;
                retrieval::SparseHit h;
                h.child_id = block.child_id;
                h.score = it.raw_score;
                by_child.emplace(block.child_id,
                                 ChunkRow{block.content_text, block.metadata_json,
                                          block.doc_id, block.score_signals});
                hits.push_back(std::move(h));
            }
            return hits;
        };

        retrieval::FivePathInput rrf_in;
        rrf_in.dense = to_child_hits(vector_result);
        rrf_in.fts5 = to_child_hits(bm25_result);

        // F40 sparse path (§6.3): embed the query's sparse vector and search the
        // per-NS SPLADE inverted index. A NS with no indexed sparse vectors (or a
        // missing registry / index open failure) yields an empty list → the fusion
        // degrades to dense+FTS5 (F40 §7.2 L2 fallback), no error.
        if (ctx.enable_sparse && sparse_registry_ != nullptr) {
            retrieval::ISparseRetriever* sparse = sparse_registry_->GetOrOpen(namespace_id);
            if (sparse != nullptr && sparse->IsAvailable()) {
                EmbedWithSparseResult sp;
                if (embedder_.EmbedWithSparse(ctx.query, &sp).ok() && !sp.sparse.empty()) {
                    retrieval::SparseVector qvec;
                    qvec.terms = sp.sparse;
                    std::vector<retrieval::SparseHit> sparse_hits =
                        sparse->Search(qvec, namespace_id, candidate_k);
                    // Resolve text for sparse-only children (no dense/fts5 hit) via the
                    // ChunkStore reverse-lookup over this NS's blocks table.
                    cortrix::store::SqliteChunkStore chunk_store(store.db_handle());
                    for (const auto& h : sparse_hits) {
                        if (by_child.find(h.child_id) == by_child.end()) {
                            auto rec = chunk_store.Get(h.child_id);
                            if (!rec.ok()) continue;  // not resolvable → drop from fusion
                            by_child.emplace(h.child_id,
                                             ChunkRow{rec.value().content, std::string(),
                                                      std::string(),
                                                      rec.value().score_signals});
                        }
                        rrf_in.sparse.push_back(h);
                    }
                }
            }
        }

        // chunk-level multi-path RRF (F40 §9.1). contextualized (F35) + hype (F38)
        // are fed empty this round per the design (mocked until those paths wire in).
        std::vector<retrieval::RrfFusedHit> fused =
            retrieval::FuseFivePathRrf(rrf_in, candidate_k);

        // 3. Assemble RankedChunks from the fused child_ids using the block cache.
        std::vector<RankedChunk> ranked;
        ranked.reserve(fused.size());
        for (const auto& fh : fused) {
            auto cit = by_child.find(fh.child_id);
            if (cit == by_child.end()) continue;
            const ChunkRow& row = cit->second;
            RankedChunk rc;
            rc.child_id = fh.child_id;
            rc.chunk_text = row.content_text;
            // parent_text reverse-lookup (F34 ParentChunkStore) is a separate seam;
            // left empty here per RETRIEVAL_TYPES_SPEC §1 ("empty until F34 wires in").
            // RankedChunk carries no parent_id field — ToResultItem sets ResultItem.
            // parent_id to its default until that F34 reverse-lookup lands (D3.5+).
            rc.score = fh.rrf_score;        // pre-rerank (multi-path RRF) score
            rc.rerank_score = fh.rrf_score;  // overwritten below when reranking
            rc.score_signals = row.score_signals;
            // Flatten metadata into TOP-LEVEL result-metadata keys (e.g. beir_corpus_id),
            // not a single opaque "metadata_json" blob — the cross-NS runner matches qrels
            // on those top-level keys (FiQA identity). Doc-level metadata (caller-supplied
            // on the row) is the base; block-level metadata overlays it (block wins on
            // collision), matching post_filter's doc→block precedence.
            const DocFields& df = doc_fields(row.doc_id);
            FlattenMetadataIntoMap(df.metadata_json, rc.metadata);
            FlattenMetadataIntoMap(row.metadata_json, rc.metadata);
            // Surface the friendly document source_path (original filename) as the
            // authoritative source name — the block only carries the internal materialize
            // name, so this overrides it so callers (search UI, chat citations) show the
            // real file name.
            if (!df.source_path.empty()) rc.metadata["source_path"] = df.source_path;
            if (!row.doc_id.empty()) {
                rc.metadata["doc_id"] = row.doc_id;
                rc.metadata["source_doc_id"] = row.doc_id;
            }
            ranked.push_back(std::move(rc));
        }

        // 4. Rerank (shared ScoreBatch + F02 RerankerScoreFusion ordering) when the
        //    request asks for it; otherwise keep the RRF order (RRF fallback path).
        if (ctx.rerank && !ranked.empty()) {
            std::vector<const char*> passages;
            passages.reserve(ranked.size());
            for (const auto& rc : ranked) passages.push_back(rc.chunk_text.c_str());
            std::vector<float> scores =
                reranker_->ScoreBatch(ctx.query.c_str(), passages);
            for (std::size_t i = 0; i < ranked.size() && i < scores.size(); ++i) {
                ranked[i].rerank_score = scores[i];
            }
            // Fused ordering score = rerank*0.7 + rrf*0.3 (F02 §4.2-ter), local
            // sort-use only (not written into RankedChunk).
            const reranker::RerankerScoreFusion fusion;
            std::vector<std::pair<float, std::size_t>> order;
            order.reserve(ranked.size());
            for (std::size_t i = 0; i < ranked.size(); ++i) {
                const float f = fusion.ComputeRerankRrfScore(
                    ranked[i].rerank_score, ranked[i].score, ranked[i], ctx.query);
                ranked[i].score = f;
                order.emplace_back(f, i);
            }
            std::stable_sort(order.begin(), order.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<RankedChunk> sorted;
            sorted.reserve(ranked.size());
            for (const auto& [f, idx] : order) {
                (void)f;
                sorted.push_back(std::move(ranked[idx]));
            }
            ranked = std::move(sorted);
        } else {
            std::stable_sort(ranked.begin(), ranked.end(),
                             [](const RankedChunk& a, const RankedChunk& b) {
                                 return a.rerank_score > b.rerank_score;
                             });
        }

        // 5. Truncate to the caller's top_k (over-fetch was reranker recall only).
        const int top_k = ctx.top_k < 1 ? 1 : ctx.top_k;
        if (static_cast<int>(ranked.size()) > top_k) {
            ranked.resize(static_cast<std::size_t>(top_k));
        }

        out.chunks = std::move(ranked);
        out.error_code.clear();  // success
        finish_latency();
        return out;
    } catch (const std::exception&) {
        const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
        out.chunks.clear();
        out.error_code = info.cx_code;
        out.error_category = agent_friendly::ToString(info.category);
        out.retryable = info.retryable;
        out.structured_data = {{"index_state", "query_failed"}};  // GEN-Agent #5
        finish_latency();
        return out;
    }
}

NamespaceQueryResult LiveSingleUnitExecutor::ExecuteDocRetrieval(
    const QueryContext& ctx, const std::string& namespace_id) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    NamespaceQueryResult out;
    out.namespace_id = namespace_id;
    auto finish_latency = [&]() {
        out.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0)
                .count());
    };

    try {
        // Acquire the NS façade (F05 Pool.Acquire, RAII-released at scope exit) — same
        // contract as the chunk path; a missing NS / store fault folds in-band as
        // CX_ERR_INDEX_CORRUPT (topic 2.4 partial success), never thrown.
        cortrix::resource::NamespaceFacade facade(pool_, namespace_id);
        if (!facade.Acquire().ok()) {
            const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
            out.error_code = info.cx_code;
            out.error_category = agent_friendly::ToString(info.category);
            out.retryable = info.retryable;
            out.structured_data = {{"index_state", "unavailable"}};  // GEN-Agent #5 (non-leaky)
            finish_latency();
            return out;
        }

        const int top_k = ctx.top_k < 1 ? 1 : ctx.top_k;

        cortrix::doc_summary::DocDiscoveryCoreOptions core_opts;
        core_opts.top_k = top_k;
        core_opts.fts5_fallback_enabled = doc_fts5_fallback_enabled_;
        cortrix::doc_summary::DocDiscoveryCoreResult core =
            cortrix::doc_summary::RunDocDiscoveryCore(
                facade.vec_index(), facade.store(), embedder_, ctx.query, core_opts);

        // Convert each doc-discovery hit into a doc-level RankedChunk. child_id = doc_id
        // (doc-level identity for this path). llm_summary hits carry summary_text;
        // fts5_fallback hits may only carry document metadata, so they become a
        // doc-level pseudo-result with the best available title/filename text.
        out.chunks.reserve(core.hits.size());
        for (const auto& h : core.hits) {
            RankedChunk rc;
            rc.child_id = h.doc_id;             // doc-level identity for this path
            rc.score = h.match_score;           // doc-discovery RRF score
            rc.rerank_score = h.match_score;    // no rerank on the doc path → mirror score
            rc.metadata["via_path"] = h.via_path;
            rc.metadata["doc_discovery_via_path"] = h.via_path;
            rc.metadata["doc_id"] = h.doc_id;
            rc.metadata["source_doc_id"] = h.doc_id;
            rc.metadata["doc_discovery_match_score"] = std::to_string(h.match_score);
            if (h.via_path == "llm_summary") {
                rc.metadata["doc_summary_match_score"] = std::to_string(h.match_score);
            } else if (h.via_path == "fts5_fallback") {
                rc.metadata["doc_fts5_match_score"] = std::to_string(h.match_score);
            }
            if (!h.one_liner.empty()) rc.metadata["one_liner"] = h.one_liner;

            CortrixDoc doc;
            if (facade.store().doc_get(h.doc_id, doc) == 0) {
                FlattenMetadataIntoMap(doc.metadata_json, rc.metadata);
                if (!doc.source_path.empty()) rc.metadata["source_path"] = doc.source_path;
                if (!doc.title.empty()) rc.metadata["doc_title"] = doc.title;
            }
            if (!h.filename.empty() && rc.metadata.find("source_path") == rc.metadata.end()) {
                rc.metadata["source_path"] = h.filename;
            }
            if (!h.doc_title.empty()) rc.metadata["doc_title"] = h.doc_title;

            if (!h.summary_text.empty()) {
                rc.chunk_text = h.summary_text;
            } else if (!h.doc_title.empty()) {
                rc.chunk_text = h.doc_title;
            } else if (rc.metadata.find("doc_title") != rc.metadata.end()) {
                rc.chunk_text = rc.metadata["doc_title"];
            } else if (rc.metadata.find("source_path") != rc.metadata.end()) {
                rc.chunk_text = rc.metadata["source_path"];
            } else {
                rc.chunk_text = h.doc_id;
            }
            // Re-assert internal provenance after flattening caller-supplied document
            // metadata so a user metadata key named doc_id/source_doc_id/via_path cannot
            // break query-time candidate identity.
            rc.metadata["via_path"] = h.via_path;
            rc.metadata["doc_discovery_via_path"] = h.via_path;
            rc.metadata["doc_id"] = h.doc_id;
            rc.metadata["source_doc_id"] = h.doc_id;
            rc.metadata["doc_discovery_match_score"] = std::to_string(h.match_score);
            if (h.via_path == "llm_summary") {
                rc.metadata["doc_summary_match_score"] = std::to_string(h.match_score);
            } else if (h.via_path == "fts5_fallback") {
                rc.metadata["doc_fts5_match_score"] = std::to_string(h.match_score);
            }
            out.chunks.push_back(std::move(rc));
        }

        out.error_code.clear();  // success (an empty doc-summary set is still success)
        finish_latency();
        return out;
    } catch (const std::exception&) {
        const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
        out.chunks.clear();
        out.error_code = info.cx_code;
        out.error_category = agent_friendly::ToString(info.category);
        out.retryable = info.retryable;
        out.structured_data = {{"index_state", "query_failed"}};  // GEN-Agent #5
        finish_latency();
        return out;
    }
}

NamespaceQueryResult LiveSingleUnitExecutor::ExecuteHybridRetrieval(
    const QueryContext& ctx, const std::string& namespace_id, float oversample) {
    // §8.1 hybrid branch: run BOTH paths and fuse their ranked lists with RRF before
    // the per-NS top_k cap. This keeps doc-summary candidates from being suppressed by
    // a chunk-first concatenation while avoiding raw-score comparisons across different
    // scoring scales. A per-path NS failure (error_code set) is surfaced if BOTH fail; if
    // only one fails we still return the other's results (partial success).
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    NamespaceQueryResult chunk_part = ExecuteChunkRetrieval(ctx, namespace_id, oversample);
    NamespaceQueryResult doc_part = ExecuteDocRetrieval(ctx, namespace_id);
    auto elapsed_ms = [&]() {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0)
                .count());
    };

    NamespaceQueryResult out;
    out.namespace_id = namespace_id;
    out.latency_ms = elapsed_ms();

    const bool chunk_ok = chunk_part.error_code.empty();
    const bool doc_ok = doc_part.error_code.empty();
    if (!chunk_ok && !doc_ok) {
        // Both failed → surface the chunk path's error (the primary path).
        out.error_code = chunk_part.error_code;
        out.error_category = chunk_part.error_category;
        out.retryable = chunk_part.retryable;
        out.retry_after_ms = chunk_part.retry_after_ms;
        out.structured_data = std::move(chunk_part.structured_data);
        return out;
    }

    if (!chunk_ok) {
        out.chunks = std::move(doc_part.chunks);
        out.error_code.clear();
        out.latency_ms = elapsed_ms();
        return out;
    }
    if (!doc_ok) {
        out.chunks = std::move(chunk_part.chunks);
        out.error_code.clear();
        out.latency_ms = elapsed_ms();
        return out;
    }

    out.chunks = FuseHybridChunksByDocId(std::move(chunk_part.chunks),
                                         std::move(doc_part.chunks),
                                         ctx.top_k);
    out.error_code.clear();  // success (at least one path succeeded)
    out.latency_ms = elapsed_ms();
    return out;
}

}  // namespace cortrix::query
