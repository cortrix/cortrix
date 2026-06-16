#include <cstdint>
#include "cortrix/spc/spc_pipeline.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <utility>
#include <unordered_map>

#include "cortrix/spc/parser_factory.h"          // F06 cortrix::spc::DocumentParserFactory
#include "cortrix/spc/parser.h"                   // ParsedDoc / ParserOptions / DocumentMetadata
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/spc_router.h"
#include "cortrix/chunker/parent_child_chunker.h" // F34 ParentChildChunker
#include "cortrix/chunker/i_chunker.h"            // ChunkerInput / ChunkerOutput
#include "cortrix/metadata/metadata_types.h"      // F08 GeneratorInput / MetadataBlock
#include "cortrix/spc/data_cleaner.h"              // F10 DataCleaner / spc::Block / ShouldSkipIndex
#include "cortrix/spc_enricher.h"                  // F03 ISpcEnricher / CreateEnricher / EnrichResult
#include "cortrix/spc/enricher_chain.h"            // I1 EnricherChain (F03→F35→F38 fail-soft serial)
#include "cortrix/spc/hype_block.h"                // I3 BuildHypeQuestionBlock / FillHypeEmbedding (F38)
#include "cortrix/spc_enricher/enricher_store.h"   // F03 WriteEnrichment (enriched_score + entities persist)
#include "cortrix/spc/contextual_store.h"          // I2 WriteContextualized (F35 contextualized_* cols)
#include "cortrix/retrieval/sparse_codec.h"        // Q4 F40 SparseVector
#include "cortrix/retrieval/sparse_index_registry.h"  // Q4 F40 per-NS index registry
#include "cortrix/retrieval/sparse_retriever.h"    // Q4 F40 ISparseRetriever::Add
#include "cortrix/retrieval/sparse_vec_store.h"    // Q4 F40 WriteSparseVec (blocks.sparse_vec)
#include "cortrix/scoring/score_map.h"             // F07 ScoringInput (parser/enricher/anomaly signals)
#include "cortrix/scoring/scoring_store.h"          // F07 WriteScore (semantic_score persist)
#include "cortrix/resource/namespace_facade.h"    // D3.5 wire⑤: Process over the façade
#include "cortrix/store/write_coordinator.h"      // C2: BeginWrite/Commit/Rollback
#include "cortrix/store/iindex.h"                 // M3: AddPoints
#include "cortrix/id/hash.h"                      // block_id = HashChildIdToBlockId (memory block)
#include "cortrix/id/ulid.h"                      // child_id ULID (memory block)
#include "cortrix/async/task_info.h"          // ⑤c SubmitRequest + kTaskDocSummary
#include "cortrix/common/block_types.h"
#include "cortrix/common/block_header.h"
#include "cortrix/logging/logging.h"

namespace cortrix {

namespace {

// Serialize a F06 DocumentMetadata into the JSON shape the parents/children rows
// carry (mirrors sqlite_parent_chunk_store.cpp MetaToJson, so the per-parent /
// per-child metadata_json round-trips with the F06 SoT field names). F03/F08
// enrich this same blob per-parent (NER/Summary).
std::string MetaToJson(const cortrix::spc::DocumentMetadata& m) {
    nlohmann::json j;
    j["filename"] = m.filename;
    j["doc_title"] = m.doc_title;
    j["mime_type"] = m.mime_type;
    j["file_size_bytes"] = m.file_size_bytes;
    j["page_count"] = m.page_count;
    j["doc_language"] = m.doc_language;
    j["upload_timestamp"] = m.upload_timestamp;
    j["parse_time_ms"] = m.parse_time_ms;
    return j.dump();
}

// Convert a chunker ParentChunk → storage CortrixParent (its DocumentMetadata
// serialized into the opaque metadata_json; the non-persisted child_ids dropped),
// so the store interface stays free of chunker / parser / id-strong-type deps.
CortrixParent ToCortrixParent(const cortrix::chunker::ParentChunk& p) {
    CortrixParent out;
    out.parent_id = p.parent_id;
    out.doc_id = p.doc_id;
    out.namespace_id = p.namespace_id;
    out.parent_text = p.parent_text;
    out.token_count = static_cast<int64_t>(p.token_count);
    out.page_start = static_cast<int64_t>(p.page_start);
    out.page_end = static_cast<int64_t>(p.page_end);
    out.byte_offset_start = static_cast<int64_t>(p.byte_offset_start);
    out.byte_offset_end = static_cast<int64_t>(p.byte_offset_end);
    out.metadata_json = MetaToJson(p.metadata);
    out.created_at = p.created_at;
    return out;
}

// Map an ISpcEnricher::Name() / EnrichResult.enricher_name (e.g. "LlmEnricher") to
// F07 ScoreMap::EnricherLevel's short token ("llm"/"contextual"/"hype"). V1's only
// real SPC-chain enricher is LlmEnricher; Null/unknown → "" (enricher_level 0). F35
// (contextual) / F38 (hype) map here when they join the chain (D3.5+).
std::string F07EnricherToken(const std::string& enricher_name) {
    if (enricher_name == "LlmEnricher") return "llm";
    if (enricher_name == "ContextualRetrievalEnricher") return "contextual";
    if (enricher_name == "HyPEEnricher") return "hype";
    return "";
}

}  // namespace

SPCPipeline::SPCPipeline(cortrix::spc::DocumentParserFactory& parser_factory,
                         cortrix::chunker::ParentChildChunker& chunker,
                         OnnxEmbedder& embedder,
                         BlockAssembler& assembler,
                         cortrix::spc::ISpcEnricher& enricher)
    : parser_factory_(parser_factory),
      chunker_(chunker),
      embedder_(embedder),
      assembler_(assembler),
      enricher_(enricher) {}

int SPCPipeline::Process(SPCTask& task, resource::NamespaceFacade& facade) {
    // Stage 1: Route
    CortrixBlockType block_type = static_cast<CortrixBlockType>(
        SPCRouter::InferBlockType(task.mime_type));
    bool is_memory = (task.source_type == "memory_session");

    // Determine processing level from task (D3: L0 skip, L1 metadata only,
    // L2 chunked, L3 full vector).
    uint8_t level = task.processing_level;

    // L0: Skip entirely — no processing.
    if (level == 0) {
        task.stage = SPCStage::kDone;
        facade.store().doc_update_status(task.doc_id, DocStatus::kReady);
        return 0;
    }

    // --- Memory-session fast path (Inc 4-3 item 6) ---
    // MemoryWriter puts Q+A text in content_hash and metadata in metadata_json.
    // BYPASS parse + chunker: assemble ONE flat block via the assembler's flat
    // Assemble() (memory has no parent), embed it, write via the unified F25 flow.
    if (is_memory) {
        if (task.cancelled.load()) {
            task.stage = SPCStage::kCancelled;
            return -1;
        }
        const std::string& text = task.content_hash;  // Q+A: "query\n---\nresponse"
        if (text.empty()) {
            facade.store().doc_update_status(task.doc_id, DocStatus::kReady);
            task.stage = SPCStage::kDone;
            return 0;
        }

        task.stage = SPCStage::kEmbedding;
        EmbeddingResult emb;
        std::vector<EmbeddingResult> embeddings;
        Status es = embedder_.EmbedBatch({text}, &embeddings);
        if (!es.ok()) {
            task.stage = SPCStage::kError;
            task.error_message = "embedding failed: " + es.message();
            return -1;
        }
        if (!embeddings.empty()) emb = embeddings[0];

        task.stage = SPCStage::kAssembling;
        ChunkResult chunk;
        chunk.text = text;
        chunk.chunk_index = 0;
        CortrixBlock block = assembler_.Assemble(
            task.doc_id, chunk, emb, kBlockMemory, /*processing_level=*/3,
            task.metadata_json);

        std::vector<std::pair<const float*, uint64_t>> vec_points;
        if (!emb.vector.empty()) {
            vec_points.push_back({emb.vector.data(), block.block_id});
        }

        store::TxnHandle txn;
        Status bw = facade.write_coordinator().BeginWrite(
            task.doc_id, {block.block_id}, /*writes_blob=*/false, &txn);
        if (!bw.ok()) {
            task.stage = SPCStage::kError;
            task.error_message = "BeginWrite (memory): " + bw.message();
            return -1;
        }
        if (!vec_points.empty()) {
            Status av = facade.vec_index().AddPoints(vec_points);
            if (!av.ok()) {
                facade.write_coordinator().Rollback(txn);
                task.stage = SPCStage::kError;
                task.error_message = "AddPoints: " + av.message();
                return -1;
            }
        }
        if (facade.store().block_insert(block) != 0) {
            facade.write_coordinator().Rollback(txn);
            task.stage = SPCStage::kError;
            task.error_message = "block_insert failed for memory block";
            return -1;
        }
        Status cm = facade.write_coordinator().Commit(txn);
        if (!cm.ok()) {
            task.stage = SPCStage::kError;
            task.error_message = "Commit (memory): " + cm.message();
            return -1;
        }
        facade.store().doc_update_status(task.doc_id, DocStatus::kReady);
        task.stage = SPCStage::kDone;
        return 0;
    }

    // L1: Metadata only — produce 1 block with metadata, no parse/chunk/embed.
    // Wrapped in a WriteCoordinator txn (C2) for crash consistency; no vector and
    // no blob → writes_blob=false. (Inc 4-3 item 7: L1 unchanged.)
    if (level == 1) {
        if (task.cancelled.load()) {
            task.stage = SPCStage::kCancelled;
            return -1;
        }
        task.stage = SPCStage::kAssembling;

        // Create a single metadata-only block (no text, no vector).
        CortrixBlock block;
        block.doc_id = task.doc_id;
        block.chunk_index = 0;
        block.block_type = static_cast<int>(block_type);
        block.processing_level = 1;
        block.block_id = id::HashChildIdToBlockId(id::GenerateUlid());  // real id (wire⑤)
        block.data = BlockBuild(
            block_type, kLevelL1, "", task.metadata_json, "", 0, 0);

        store::TxnHandle txn;
        Status bw = facade.write_coordinator().BeginWrite(
            task.doc_id, {block.block_id}, /*writes_blob=*/false, &txn);
        if (!bw.ok()) {
            task.stage = SPCStage::kError;
            task.error_message = "BeginWrite (L1): " + bw.message();
            return -1;
        }
        int rc = facade.store().block_insert(block);
        if (rc != 0) {
            facade.write_coordinator().Rollback(txn);
            task.stage = SPCStage::kError;
            task.error_message = "block_insert failed for L1 metadata block";
            return -1;
        }
        Status cm = facade.write_coordinator().Commit(txn);
        if (!cm.ok()) {
            task.stage = SPCStage::kError;
            task.error_message = "Commit (L1): " + cm.message();
            return -1;
        }

        facade.store().doc_update_status(task.doc_id, DocStatus::kReady);
        task.stage = SPCStage::kDone;
        return 0;
    }

    // Stage 2: Parse (F06 structured parser — owns scan detection / OCR / fallback
    // internally; DECISION X). (Inc 4-3 item 2.)
    if (task.cancelled.load()) {
        task.stage = SPCStage::kCancelled;
        return -1;
    }
    task.stage = SPCStage::kParsing;

    // ParserOptions defaults are the F12-resolved CE values (max_pages / file-size
    // limits / OCR fallback on); a per-task / NS override merge is a separate D3.5
    // item, so use defaults here.
    cortrix::spc::ParserOptions opts;
    cortrix::spc::ParsedDoc d = parser_factory_.ParseDocument(task.source_path, opts);
    if (!d.ok()) {
        task.stage = SPCStage::kError;
        task.error_message = "parse failed: " + d.error_msg;
        return -1;
    }

    // [Plan B · F42 §4.1.2] Hand the parsed doc to the post-parse stages. The F42 async
    // path reaches the same stages via ProcessParsed() directly (parse done by F06).
    return ProcessParsed(d, task, facade);
}

int SPCPipeline::ProcessParsed(cortrix::spc::ParsedDoc& d, SPCTask& task,
                               resource::NamespaceFacade& facade) {
    // Re-derive the Stage-1 routing locals the post-parse stages read (Process computed
    // these before Stage 2; ProcessParsed is also reached directly from the F42 async path).
    uint8_t level = task.processing_level;
    CortrixBlockType block_type = static_cast<CortrixBlockType>(
        SPCRouter::InferBlockType(task.mime_type));

    // Stage 3: Chunk (F34 parent-child). (Inc 4-3 item 3.)
    if (task.cancelled.load()) {
        task.stage = SPCStage::kCancelled;
        return -1;
    }
    task.stage = SPCStage::kChunking;

    cortrix::chunker::ChunkerInput in;
    in.doc_id = task.doc_id;
    in.namespace_id = facade.namespace_id();
    in.pages = std::move(d.pages);
    in.metadata = d.metadata;

    auto chunked = chunker_.Chunk(in);
    if (!chunked.ok()) {
        // Empty document is not a hard failure for the pipeline: mark ready with 0
        // blocks (mirror the MVP empty-text handling). Other chunk errors → error.
        const std::string& msg = chunked.status().message();
        if (msg.find("EMPTY_DOCUMENT") != std::string::npos ||
            msg.find("empty") != std::string::npos) {
            facade.store().doc_update_status(task.doc_id, DocStatus::kReady);
            task.stage = SPCStage::kDone;
            return 0;
        }
        task.stage = SPCStage::kError;
        task.error_message = "chunk failed: " + msg;
        return -1;
    }
    cortrix::chunker::ChunkerOutput& out = chunked.value();

    // Both empty → ready with 0 blocks (mirror existing empty-text handling).
    if (out.parents.empty() && out.children.empty()) {
        facade.store().doc_update_status(task.doc_id, DocStatus::kReady);
        task.stage = SPCStage::kDone;
        return 0;
    }

    // Stage 3.5: F08 document-level Metadata Block GENERATION (D6 lock F06→F34→F08;
    // ARCH §2.3 SoT — F08 runs after chunk, before enrich/embed). Generate ONLY here;
    // the META block is embedded in Stage 4 and assembled into the write set after the
    // child loop. META gen is non-fatal (a truly empty doc logs + skips; parents/children
    // stay valid). One META per doc is enforced by idx_blocks_meta_doc (F34SchemaProvider).
    std::optional<cortrix::metadata::MetadataBlock> meta_mb;
    {
        cortrix::metadata::GeneratorInput gin;
        gin.doc_id = task.doc_id;
        gin.namespace_id = facade.namespace_id();
        gin.doc_metadata = in.metadata;
        gin.file_info.source_uri = task.source_path;
        gin.file_info.upload_time_ms = in.metadata.upload_timestamp;
        gin.stats.page_count = in.metadata.page_count;
        gin.stats.chunk_count = static_cast<int>(out.children.size());
        gin.stats.parent_count = static_cast<int>(out.parents.size());
        gin.stats.child_count = static_cast<int>(out.children.size());
        gin.stats.processing_time_ms = in.metadata.parse_time_ms;
        gin.stats.parser_name = d.parser_name;
        gin.stats.chunker_name = "parent-child";
        gin.stats.parse_status = d.failed_pages.empty() ? "ok" : "partial";
        if (!task.metadata_json.empty()) {
            nlohmann::json cm =
                nlohmann::json::parse(task.metadata_json, nullptr, /*allow_exceptions=*/false);
            if (!cm.is_discarded() && cm.is_object()) gin.custom_metadata = std::move(cm);
        }
        auto gres = meta_generator_.Generate(gin);
        if (!gres.ok()) {
            CORTRIX_LOG_WARN("spc", "F08 metadata gen skipped for doc_id={}: {}",
                             task.doc_id, gres.status().message());
        } else {
            meta_mb = std::move(gres.value().block);
        }
    }

    // Stage 3.6: enrich (ISpcEnricher chain; ARCH §2.3 SoT — after F08, before embed,
    // so F35 contextual-retrieval can re-embed enriched text). Build a ChunkContext per
    // child (text + F06 doc_meta + prev/next neighbors), enrich, and stash each result by
    // child_id. F03 summary → child metadata_json at assembly; enriched_score + entities +
    // F35 contextualized_* persist in the write phase. Enrich needs no embedding, so it
    // precedes Stage 4. Runs for L2/L3 alike (both produce children).
    //
    // [I1 · GS-2] When the enricher chain is installed (bootstrap, F03→F35→F38 fail-soft
    // serial) it supersedes the single ctor enricher_: EnrichChunks() returns one merged
    // EnrichResult per child (F03 entities/summary + F35 contextualized_*) PLUS the F38
    // hype_question side channel. Unset / no chain member available → the single-enricher
    // path (backward compatible). NullEnricher short-circuits → the maps stay empty.
    std::unordered_map<std::string, cortrix::spc::EnrichResult> enrich_by_child;
    // [I3] F38 hype questions per source child (the write phase builds block_type=16 Blocks).
    std::unordered_map<std::string, std::vector<cortrix::spc::HypeQuestion>> hype_by_child;
    const bool use_chain = enricher_chain_ && enricher_chain_->AnyAvailable();
    if ((use_chain || enricher_.IsAvailable()) && !out.children.empty()) {
        if (task.cancelled.load()) {
            task.stage = SPCStage::kCancelled;
            return -1;
        }
        std::vector<cortrix::spc::ChunkContext> ctxs;
        ctxs.reserve(out.children.size());
        for (size_t i = 0; i < out.children.size(); ++i) {
            cortrix::spc::ChunkContext c;
            c.chunk_text = out.children[i].child_text;
            c.chunk_index = static_cast<int>(out.children[i].chunk_index);
            c.doc_metadata = &in.metadata;  // F06 SoT; `in` is function-scope → outlives this call
            c.prev_chunk_text = (i == 0) ? "" : out.children[i - 1].child_text;
            c.next_chunk_text =
                (i + 1 == out.children.size()) ? "" : out.children[i + 1].child_text;
            ctxs.push_back(std::move(c));
        }

        if (use_chain) {
            // [I3] Real chunk→parent binding: resolve each child's parent_text from the
            // in-memory parents (F38 reconcile 2 — parent_text is optional context). The
            // source_child_id / source_parent_id provenance is stamped on each generated
            // hype question (F38-4).
            std::unordered_map<std::string, const std::string*> parent_text_by_id;
            for (const auto& p : out.parents) parent_text_by_id[p.parent_id] = &p.parent_text;
            std::vector<std::string> parent_texts(out.children.size());
            std::vector<std::string> src_child_ids(out.children.size());
            std::vector<std::string> src_parent_ids(out.children.size());
            for (size_t i = 0; i < out.children.size(); ++i) {
                const auto& ch = out.children[i];
                src_child_ids[i] = ch.child_id;
                src_parent_ids[i] = ch.parent_id;
                auto pit = parent_text_by_id.find(ch.parent_id);
                if (pit != parent_text_by_id.end() && pit->second) parent_texts[i] = *pit->second;
            }
            std::vector<cortrix::spc::ChunkChainResult> chain_res =
                enricher_chain_->EnrichChunks(ctxs, parent_texts, src_child_ids, src_parent_ids);
            for (size_t i = 0; i < chain_res.size() && i < out.children.size(); ++i) {
                enrich_by_child.emplace(out.children[i].child_id,
                                        std::move(chain_res[i].merged));
                if (!chain_res[i].hype_questions.empty()) {
                    hype_by_child.emplace(out.children[i].child_id,
                                          std::move(chain_res[i].hype_questions));
                }
            }
        } else {
            // Single-enricher path (backward compatible). EnrichBatch returns one result per
            // context, aligned by index; re-key by child_id so the write phase attaches each
            // result to its block regardless of F10 dedup reordering.
            std::vector<cortrix::spc::EnrichResult> eres = enricher_.EnrichBatch(ctxs);
            for (size_t i = 0; i < eres.size() && i < out.children.size(); ++i) {
                enrich_by_child.emplace(out.children[i].child_id, std::move(eres[i]));
            }
        }
    }

    // Stage 4: Embed children (L3 only; L2 skips embedding). (Inc 4-3 item 4.)
    // [Q4 · F40] When the sparse registry is wired, use EmbedBatchWithSparse so the
    // single BGE-M3 pass yields BOTH the dense embedding (unchanged downstream) AND
    // the SPLADE sparse vector per child (persisted + indexed in the write phase).
    // sparse_by_child holds the per-child SparseVector, keyed by child_id.
    std::vector<EmbeddingResult> embeddings;
    std::unordered_map<std::string, cortrix::retrieval::SparseVector> sparse_by_child;
    if (level >= 3 && !out.children.empty()) {
        if (task.cancelled.load()) {
            task.stage = SPCStage::kCancelled;
            return -1;
        }
        task.stage = SPCStage::kEmbedding;

        std::vector<std::string> child_texts;
        child_texts.reserve(out.children.size());
        for (const auto& c : out.children) child_texts.push_back(c.child_text);

        if (sparse_registry_ != nullptr) {
            std::vector<EmbedWithSparseResult> sp_results;
            Status embed_status =
                embedder_.EmbedBatchWithSparse(child_texts, &sp_results);
            if (!embed_status.ok()) {
                task.stage = SPCStage::kError;
                task.error_message = "embedding failed: " + embed_status.message();
                return -1;
            }
            embeddings.reserve(sp_results.size());
            for (std::size_t i = 0; i < sp_results.size(); ++i) {
                EmbeddingResult er;
                er.vector = std::move(sp_results[i].dense);
                er.dim = static_cast<int>(er.vector.size());
                embeddings.push_back(std::move(er));
                if (i < out.children.size() && !sp_results[i].sparse.empty()) {
                    cortrix::retrieval::SparseVector sv;
                    sv.terms = std::move(sp_results[i].sparse);
                    sparse_by_child[out.children[i].child_id] = std::move(sv);
                }
            }
        } else {
            Status embed_status = embedder_.EmbedBatch(child_texts, &embeddings);
            if (!embed_status.ok()) {
                task.stage = SPCStage::kError;
                task.error_message = "embedding failed: " + embed_status.message();
                return -1;
            }
        }
    }

    // Stage 4 (cont.): embed the F08 META block_text (doc-level; its own embedding).
    // Declared at function scope so it outlives the vec_points pointer into it.
    EmbeddingResult meta_embedding;
    if (meta_mb && level >= 3) {
        std::vector<EmbeddingResult> tmp;
        Status es = embedder_.EmbedBatch({meta_mb->block_text}, &tmp);
        if (es.ok() && !tmp.empty()) meta_embedding = std::move(tmp[0]);
        // embed failure for META → non-fatal: write the block without a vector.
    }

    // Stage 5: Assemble + Store, transactionally (C2 / design §10.2). (Inc 4-3 item 5.)
    if (task.cancelled.load()) {
        task.stage = SPCStage::kCancelled;
        return -1;
    }
    task.stage = SPCStage::kAssembling;

    // modality = the document's source block type (candidate B: child-ness is
    // carried by child_id, not a kBlockChild enum).
    const CortrixBlockType modality = block_type;

    // Phase 1 — F10 data cleaning + assembly. Build a lightweight Block view of the
    // children (id + text + the upstream embedding + inherited metadata), run F10
    // dedup (exact SHA-256 + semantic cosine over the embeddings) and anomaly
    // detection, then assemble the survivors. Dedup runs AFTER embed because the
    // semantic pass needs the embeddings (F10 D3); deduped children are dropped from
    // the write (the `parents` table stores no child_ids, so no parent dangles);
    // anomalous children are still written (with "cleaning.*" in metadata_json) but
    // skipped from P-HNSW (ShouldSkipIndex, F10 D5).
    std::vector<CortrixBlock> child_blocks;
    std::vector<std::pair<const float*, uint64_t>> vec_points;
    // ④ F07: block_id → write-time semantic_score, drained in the write phase (WriteScore).
    std::unordered_map<uint64_t, float> score_by_block;
    child_blocks.reserve(out.children.size());

    std::vector<cortrix::spc::Block> clean_blocks;
    std::unordered_map<std::string, size_t> child_idx_by_id;
    clean_blocks.reserve(out.children.size());
    for (size_t i = 0; i < out.children.size(); ++i) {
        const auto& child = out.children[i];
        cortrix::spc::Block cb;
        cb.id = child.child_id;
        cb.chunk_text = child.child_text;
        if (level >= 3 && i < embeddings.size()) cb.embedding = embeddings[i].vector;
        cb.metadata_json = nlohmann::json::parse(
            MetaToJson(child.metadata), nullptr, /*allow_exceptions=*/false);
        if (cb.metadata_json.is_discarded()) cb.metadata_json = nlohmann::json::object();
        clean_blocks.push_back(std::move(cb));
        child_idx_by_id[child.child_id] = i;
    }
    data_cleaner_.Dedup(clean_blocks);          // drops exact + semantic duplicates
    data_cleaner_.DetectAnomaly(clean_blocks);  // marks skip-index (kept, not indexed)

    for (auto& b : clean_blocks) {
        const cortrix::chunker::ChildChunk& child = out.children[child_idx_by_id[b.id]];
        EmbeddingResult emb;
        emb.vector = b.embedding;
        emb.dim = static_cast<int>(b.embedding.size());
        // F03 §3.1: summary → Block.payload.metadata JSONB. Fold the enriched summary
        // (when produced) into this child's metadata before assembly so it travels in the
        // block payload; enriched_score + entities persist separately in the write phase.
        {
            auto eit = enrich_by_child.find(b.id);
            if (eit != enrich_by_child.end() && eit->second.ok() &&
                !eit->second.summary.empty()) {
                b.metadata_json["summary"] = eit->second.summary;
            }
        }
        // ④ F07 SemanticScorer (ARCH §2.3 step-7, D7 — clean→score→assemble): compute the
        // Matrix processing_level (parser/enricher/anomaly) + write-time semantic_score via
        // the frozen AssignInitialScore. A throwaway header captures the level byte (the scorer
        // only writes processing_level, reads no header field); the level is then baked into
        // BOTH the block header byte AND blocks.processing_level (option A — F07 is the sole
        // owner of block-level processing_level; ingest depth L0-L3 lives at the doc level).
        // The score travels to the write phase (WriteScore), keyed by block_id.
        std::string enr_token;
        {
            auto eit = enrich_by_child.find(b.id);
            if (eit != enrich_by_child.end() && eit->second.ok())
                enr_token = F07EnricherToken(eit->second.enricher_name);
        }
        cortrix_block_header_t f07_hdr{};
        float semantic_score = 0.0f;
        cortrix::scoring::ScoringInput sin;
        sin.parser_name = d.parser_name;
        sin.enricher_name = enr_token;
        sin.is_anomalous = cortrix::spc::ShouldSkipIndex(b);
        sin.block_type = static_cast<uint16_t>(modality);
        scorer_.AssignInitialScore(f07_hdr, semantic_score, sin);
        // [I2 · F35 §4.1] flags_ext bit3 (has_contextualized_embedding): set when F35
        // produced a contextualized embedding for this child (double-vector coexistence).
        uint8_t child_flags_ext = 0;
        {
            auto eit = enrich_by_child.find(b.id);
            if (eit != enrich_by_child.end() &&
                eit->second.contextualized_embedding.has_value() &&
                !eit->second.contextualized_embedding->empty()) {
                child_flags_ext |= kFlagExtHasContextualized;
            }
        }
        child_blocks.push_back(assembler_.AssembleChild(
            child, emb, modality, f07_hdr.processing_level, b.metadata_json.dump(),
            child_flags_ext));
        score_by_block[child_blocks.back().block_id] = semantic_score;
        if (level >= 3 && !b.embedding.empty() && !cortrix::spc::ShouldSkipIndex(b)) {
            // vec_points borrows the embedding storage inside clean_blocks (stable
            // through the write below).
            vec_points.push_back({b.embedding.data(), child_blocks.back().block_id});
        }
    }

    // [I3 · F38 §5.2/§9.1] Assemble hype_question Blocks (block_type=16) from the chain's
    // F38 side channel and append them to the SAME write set so the same BeginWrite /
    // block_insert loop persists them in ONE F25 PWL transaction with the chunk + META
    // Blocks (F25 §5.2 atomic write). Each question's embedding is the BGE-M3 vector of
    // the question text (the §9.1 `hype_q.embedding = OnnxEmbedder.Embed(question_text)`
    // step); block_id = HashChildIdToBlockId(a fresh ULID per question). The questions are
    // keyed by source child_id (provenance in metadata_json.source_child_id) so recall can
    // expand a hype hit back to its child (F38-4). L3 only (hype Blocks index into P-HNSW);
    // for L2 the questions carry no vector and are skipped (no enrich-only doc has them).
    if (level >= 3 && !hype_by_child.empty()) {
        // Flatten the question texts to embed in one batch (cheap when the chain ran).
        std::vector<cortrix::spc::HypeQuestion*> all_questions;
        std::vector<std::string> q_texts;
        for (auto& kv : hype_by_child) {
            for (auto& q : kv.second) {
                all_questions.push_back(&q);
                q_texts.push_back(q.question_text);
            }
        }
        std::vector<EmbeddingResult> q_embs;
        if (!q_texts.empty()) {
            Status qes = embedder_.EmbedBatch(q_texts, &q_embs);
            if (!qes.ok()) {
                // Non-fatal (F38-8 transparent degrade): the chunk Blocks are the primary
                // deliverable; a hype embed failure drops the hype Blocks, not the doc.
                CORTRIX_LOG_WARN("spc",
                    "F38 hype embedding failed for doc_id={}, dropping hype blocks: {}",
                    task.doc_id, qes.message());
                q_embs.clear();
            }
        }
        for (size_t i = 0; i < all_questions.size() && i < q_embs.size(); ++i) {
            cortrix::spc::HypeQuestion& q = *all_questions[i];
            q.embedding = q_embs[i].vector;
            const std::string hype_ulid = id::GenerateUlid();
            const uint64_t hype_block_id = id::HashChildIdToBlockId(hype_ulid);
            const std::string generated_at = "";  // ISO-8601 stamp is Phase-2 cost telemetry
            CortrixBlock hb;
            hb.doc_id = task.doc_id;
            hb.chunk_index = 0;
            hb.block_type = static_cast<int>(kBlockHypeQuestion);
            hb.processing_level = static_cast<int>(level);
            hb.block_id = hype_block_id;
            hb.content_text = q.question_text;
            hb.metadata_json = cortrix::spc::BuildHypeQuestionMetadataJson(
                q, /*prompt_version=*/"v1", /*llm_model=*/"gpt-4o-mini",
                /*cost_usd=*/0.0, generated_at);
            hb.data = cortrix::spc::BuildHypeQuestionBlock(
                q, /*prompt_version=*/"v1", /*llm_model=*/"gpt-4o-mini",
                /*cost_usd=*/0.0, generated_at);
            child_blocks.push_back(std::move(hb));
            if (!q.embedding.empty()) {
                vec_points.push_back({q.embedding.data(), child_blocks.back().block_id});
            }
        }
    }

    // Assemble the F08 META block from the pre-generated mb (Stage 3.5) + its Stage-4
    // embedding, appending it to the write set so the same BeginWrite/block_insert loop
    // persists it (same blocks table + same F25 txn; L3 → P-HNSW). [③a: gen+embed moved
    // earlier; here only construct.]
    if (meta_mb) {
        // ④ F07 for the META block: ComputeLevel special-cases block_type==kBlockMeta → Matrix
        // level 0 / score 0.2 (ARCH §5.2.1 lock), regardless of parser/enricher. Same dual-write
        // as children: the level goes into both the header byte and blocks.processing_level (the
        // embed gating above used the TASK level, so a META block can still carry its L3 vector).
        cortrix_block_header_t meta_hdr{};
        float meta_score = 0.0f;
        cortrix::scoring::ScoringInput msin;
        msin.parser_name = d.parser_name;
        msin.block_type = static_cast<uint16_t>(kBlockMeta);
        scorer_.AssignInitialScore(meta_hdr, meta_score, msin);

        CortrixBlock meta_block;
        meta_block.doc_id = task.doc_id;
        meta_block.chunk_index = 0;  // F08: the doc-level Chunk[0]
        meta_block.block_type = static_cast<int>(kBlockMeta);
        meta_block.processing_level = meta_hdr.processing_level;  // F07 Matrix level (0 for META)
        meta_block.block_id = id::HashChildIdToBlockId(meta_mb->block_id);
        meta_block.content_text = meta_mb->block_text;
        meta_block.metadata_json = meta_mb->metadata_json.dump();
        meta_block.data = BlockBuild(
            kBlockMeta, static_cast<ProcessingLevel>(meta_hdr.processing_level),
            meta_mb->block_text, meta_block.metadata_json, "",
            static_cast<uint16_t>(meta_embedding.dim), 0);
        score_by_block[meta_block.block_id] = meta_score;
        child_blocks.push_back(std::move(meta_block));
        if (level >= 3 && !meta_embedding.vector.empty()) {
            vec_points.push_back(
                {meta_embedding.vector.data(), child_blocks.back().block_id});
        }
    }

    // Phase 2 — transactional write. SPC never writes a blob, so writes_blob=false
    // (F25 §10.4). P-HNSW first, then SQLite (parents + child blocks) (design §4.5);
    // the rollback callback (§9.4) cleans blocks + parents idempotently on failure.
    std::vector<BlockId> child_block_ids;
    child_block_ids.reserve(child_blocks.size());
    for (const auto& b : child_blocks) child_block_ids.push_back(b.block_id);

    store::TxnHandle txn;
    Status bw = facade.write_coordinator().BeginWrite(
        task.doc_id, child_block_ids, /*writes_blob=*/false, &txn);
    if (!bw.ok()) {
        task.stage = SPCStage::kError;
        task.error_message = "BeginWrite: " + bw.message();
        return -1;
    }

    if (!vec_points.empty()) {
        Status av = facade.vec_index().AddPoints(vec_points);
        if (!av.ok()) {
            facade.write_coordinator().Rollback(txn);
            task.stage = SPCStage::kError;
            task.error_message = "AddPoints: " + av.message();
            return -1;
        }
    }

    // parents land in the `parents` table regardless of level whenever they exist
    // (Inc 4-3 item 7). Flat-fallback children have no parent row (item 8).
    for (const auto& p : out.parents) {
        CortrixParent cp = ToCortrixParent(p);
        int rc = facade.store().parent_insert(cp);
        if (rc != 0) {
            facade.write_coordinator().Rollback(txn);
            task.stage = SPCStage::kError;
            task.error_message = "parent_insert failed";
            return -1;
        }
    }

    // F03/F07: the borrowed sqlite3* for inline enrichment + semantic_score persistence.
    // nullptr for non-SQLite stores (test fakes) → those writes are skipped (the core block
    // is already inserted). The SPC write is already serialized per-NS by the F25 coordinator;
    // WriteEnrichment / WriteScore run their own statements — safe because neither block_insert
    // nor the F25 coordinator holds an open SQLite txn (F25 is a pure PWL; crash consistency =
    // compensating rollback). Both persists are NON-FATAL (mirror the F08 META-gen skip above):
    // the block row + vector are the primary deliverable, enriched_score / semantic_score are
    // auxiliary quality columns that degrade to NULL, so a failure logs and continues rather
    // than discard the whole doc (WriteEnrichment also rolls back its own partial write).
    sqlite3* store_db = facade.store().db_handle();
    for (auto& b : child_blocks) {
        int rc = facade.store().block_insert(b);
        if (rc != 0) {
            facade.write_coordinator().Rollback(txn);
            task.stage = SPCStage::kError;
            task.error_message = "block_insert failed";
            return -1;
        }
        if (store_db) {
            // F03 §3.1: enriched_score + entities, keyed by child_id (META / memory carry none).
            if (!b.child_id.empty()) {
                auto eit = enrich_by_child.find(b.child_id);
                if (eit != enrich_by_child.end() && eit->second.ok()) {
                    Status we = cortrix::spc::WriteEnrichment(store_db, b.block_id, eit->second);
                    if (!we.ok()) {
                        CORTRIX_LOG_WARN("spc",
                            "F03 enrichment persist skipped for block_id={} (doc_id={}): {}",
                            b.block_id, task.doc_id, we.message());
                    }
                }
                // [I2 · F35 §4.1] contextualized_* columns (child rows only). Gated on the
                // F35 output inside the merged result (contextualized_status / engaged
                // optionals), NOT on merged.ok() — F35 can degrade (status=failed) while F03
                // succeeded, and the §4.1 columns must still record the F35 outcome. No-op
                // when the chain had no F35 stage (columns stay NULL/0).
                if (eit != enrich_by_child.end()) {
                    Status wc = cortrix::spc::WriteContextualized(store_db, b.block_id, eit->second);
                    if (!wc.ok()) {
                        CORTRIX_LOG_WARN("spc",
                            "F35 contextualized persist skipped for block_id={} (doc_id={}): {}",
                            b.block_id, task.doc_id, wc.message());
                    }
                }
                // [Q4 · F40 §6.1] Persist the child's SPLADE sparse vector to
                // blocks.sparse_vec (durable copy / index-rebuild source) AND index
                // it into the per-NS inverted index (live serving). A child with no
                // active sparse terms (dead chunk, §6.5) writes NULL + indexes
                // nothing. Both are best-effort fail-soft: a sparse persist/index
                // fault degrades the chunk to dense+FTS5, never fails the doc write.
                if (sparse_registry_ != nullptr) {
                    auto spit = sparse_by_child.find(b.child_id);
                    const cortrix::retrieval::SparseVector empty_vec;
                    const cortrix::retrieval::SparseVector& sv =
                        spit != sparse_by_child.end() ? spit->second : empty_vec;
                    Status wsv = cortrix::retrieval::WriteSparseVec(store_db, b.block_id, sv);
                    if (!wsv.ok()) {
                        CORTRIX_LOG_WARN("spc",
                            "F40 sparse_vec persist skipped for block_id={} (doc_id={}): {}",
                            b.block_id, task.doc_id, wsv.message());
                    }
                    if (!sv.empty()) {
                        if (cortrix::retrieval::ISparseRetriever* sparse =
                                sparse_registry_->GetOrOpen(facade.namespace_id())) {
                            Status add = sparse->Add(facade.namespace_id(), b.child_id, sv);
                            if (!add.ok()) {
                                CORTRIX_LOG_WARN("spc",
                                    "F40 sparse index Add skipped for child_id={} (ns={}): {}",
                                    b.child_id, facade.namespace_id(), add.message());
                            }
                        }
                    }
                }
            }
            // F07 §5.1.2: write-time semantic_score for every F07-scored block (L2/L3 children
            // + META). Keyed by block_id (the fast L0/L1/memory paths never populate the map).
            auto sit = score_by_block.find(b.block_id);
            if (sit != score_by_block.end()) {
                Status ws = cortrix::scoring::WriteScore(store_db, b.block_id, sit->second);
                if (!ws.ok()) {
                    CORTRIX_LOG_WARN("spc",
                        "F07 semantic_score persist skipped for block_id={} (doc_id={}): {}",
                        b.block_id, task.doc_id, ws.message());
                }
            }
        }
    }

    Status cm = facade.write_coordinator().Commit(txn);
    if (!cm.ok()) {
        task.stage = SPCStage::kError;
        task.error_message = "Commit: " + cm.message();
        return -1;
    }

    // Phase 3 — document status.
    facade.store().doc_update_status(task.doc_id, DocStatus::kReady);
    task.stage = SPCStage::kDone;
    // [⑤c] F41 doc-summary enqueue seam: a fully written document fires the hook
    // (no-op unless production installed the seam via SetDocSummaryEnqueue).
    OnDocumentWritten(task.doc_id, task.namespace_name);
    return 0;
}

void SPCPipeline::SetDocSummaryEnqueue(
    std::function<void(const async::SubmitRequest&)> fn) {
    doc_summary_enqueue_ = std::move(fn);
}

void SPCPipeline::SetEnricherChain(cortrix::spc::EnricherChain* chain) {
    enricher_chain_ = chain;
}

void SPCPipeline::SetSparseIndexRegistry(
    cortrix::retrieval::SparseIndexRegistry* registry) {
    sparse_registry_ = registry;
}

void SPCPipeline::OnDocumentWritten(const std::string& doc_id,
                                    const std::string& ns_id) {
    if (!doc_summary_enqueue_) return;  // feature off (no F42 scheduler wired)
    async::SubmitRequest req;
    req.namespace_id = ns_id;
    req.doc_id = doc_id;
    // content_hash = doc_id makes a repeat OnDocumentWritten for the SAME doc within
    // the debounce window self-merge (no duplicate summary). NOTE: F42's debounce is
    // keyed on doc_id alone (TaskManager::FindRecentTaskByDocId ignores task_type +
    // terminal status), so a doc-summary enqueue can collide with the doc's own
    // (possibly completed) doc-parse task — resolving that (debounce per task_type)
    // belongs to the F42 main-wiring step (Derek 2026-06-08 chose B: F42 productization).
    req.content_hash = doc_id;
    req.task_type = async::kTaskDocSummary;
    doc_summary_enqueue_(req);
}

}  // namespace cortrix
