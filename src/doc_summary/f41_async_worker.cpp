#include <cstdint>
#include "cortrix/doc_summary/f41_async_worker.h"

#include <chrono>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/common/block_types.h"
#include "cortrix/common/data_types.h"           // CortrixBlock
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/onnx_embedder.h"           // EmbeddingResult / OnnxEmbedder
#include "cortrix/spc/recursive_chunker.h"       // ChunkResult
#include "cortrix/store/iindex.h"                // AddPoints
#include "cortrix/store/sqlite_chunk_store.h"    // ⑤a ChunkStore over blocks
#include "cortrix/store/write_coordinator.h"     // TxnHandle / BeginWrite / Commit / Rollback

namespace cortrix {
namespace doc_summary {
namespace {

// §4.2 doc_summary block metadata_json. status ∈ {pending|generated|failed}; here it
// is always "generated" — the worker writes a block only on LLM success (a failed doc
// has no block). summary_text itself rides on content_text + the embedding.
std::string BuildMetadataJson(const DocSummaryStructured& s, const DocSummaryConfig& cfg) {
    nlohmann::json j;
    j["keywords"] = s.keywords;
    j["topics"] = s.topics;
    j["one_liner"] = s.one_liner;
    j["prompt_version"] = cfg.prompt_version;
    j["llm_model"] = cfg.llm_model;
    j["status"] = "generated";
    return j.dump();
}

}  // namespace

F41AsyncWorker::F41AsyncWorker(resource::INamespacePool& pool,
                               std::shared_ptr<llm::ILlmClient> llm_client,
                               DocSummaryConfig config, OnnxEmbedder& embedder,
                               BlockAssembler& assembler, async::TaskManager* task_mgr)
    : pool_(pool),
      llm_client_(std::move(llm_client)),
      config_(std::move(config)),
      embedder_(embedder),
      assembler_(assembler),
      finalizer_(task_mgr) {}

Status F41AsyncWorker::ProcessTask(const async::TaskInfo& task) {
    const auto t_start = std::chrono::steady_clock::now();

    // (1) Acquire the task's Namespace (RAII Release on scope exit).
    resource::NamespaceFacade facade(pool_, task.namespace_id);
    Status acq = facade.Acquire();
    if (!acq.ok())
        return finalizer_.Fail(task, "CX_ERR_F41_NS_ACQUIRE_FAILED", acq.message(),
                               {{"doc_id", task.doc_id}}, t_start);

    // (2) Per-task ChunkStore over THIS Unit's blocks + generator; run the LLM summary.
    //     (DocSummaryGenerator records the llm_calls / summaries_generated metrics.)
    auto chunk_store = std::make_shared<store::SqliteChunkStore>(facade.store().db_handle());
    DocSummaryGenerator generator(config_, llm_client_, chunk_store);
    GenerationResult result = generator.Generate(task.doc_id, task.namespace_id);
    if (!result.success) {
        // No block written → doc-discovery degrades to FTS5 (§7.1); finalize failed
        // (Phase 1 terminal-on-failure). The generator already recorded its metrics.
        const std::string code =
            result.error ? result.error->code : "CX_ERR_F41_GENERATION_FAILED";
        const std::string msg =
            result.error ? result.error->message : "doc summary generation failed";
        nlohmann::json structured_data = {{"doc_id", task.doc_id}};
        if (result.error && result.error->structured_data.has_value() &&
            result.error->structured_data->is_object()) {
            structured_data = *result.error->structured_data;
            structured_data["doc_id"] = task.doc_id;
        }
        return finalizer_.Fail(task, code, msg, structured_data, t_start);
    }

    if (result.no_summary_content) {
        // Empty documents have an explicit doc_id but no summary material. Mark the
        // task complete without adding a synthetic doc_summary block.
        return finalizer_.Complete(task, task.doc_id, t_start);
    }

    // (3) Re-embed the summary_text (Generate leaves embedding empty by design).
    EmbeddingResult emb;
    Status es = embedder_.Embed(result.summary.summary_text, &emb);
    if (!es.ok())
        return finalizer_.Fail(task, "CX_ERR_F41_EMBED_FAILED", es.message(),
                               {{"doc_id", task.doc_id}}, t_start);

    // (4) Assemble the doc_summary block (block_type=17) and write it in one write-coordinator txn
    //     (BeginWrite → AddPoints[P-HNSW] → block_insert → Commit).
    const std::string metadata_json = BuildMetadataJson(result.summary, config_);
    ChunkResult chunk;
    chunk.text = result.summary.summary_text;
    chunk.chunk_index = 0;
    CortrixBlock block = assembler_.Assemble(task.doc_id, chunk, emb, kBlockDocSummary,
                                             /*processing_level=*/3, metadata_json);
    // Assemble (flat) embeds metadata_json only into `data`; doc-discovery queries the
    // blocks.metadata_json COLUMN (§4.2 keywords/topics/status), so set it explicitly
    // here. (AssembleChild already sets the column; aligning the flat Assemble's column
    // for every block_type — incl. the memory path — is a separate A-blocks #4 item.)
    block.metadata_json = metadata_json;

    std::vector<std::pair<const float*, uint64_t>> vec_points;
    if (!emb.vector.empty()) {
        vec_points.push_back({emb.vector.data(), block.block_id});
    }

    store::TxnHandle txn;
    Status bw = facade.write_coordinator().BeginWrite(task.doc_id, {block.block_id},
                                                      /*writes_blob=*/false, &txn);
    if (!bw.ok())
        return finalizer_.Fail(task, "CX_ERR_F41_WRITE_FAILED", bw.message(),
                               {{"doc_id", task.doc_id}}, t_start);

    if (!vec_points.empty()) {
        Status av = facade.vec_index().AddPoints(vec_points);
        if (!av.ok()) {
            facade.write_coordinator().Rollback(txn);
            return finalizer_.Fail(task, "CX_ERR_F41_WRITE_FAILED", av.message(),
                                   {{"doc_id", task.doc_id}}, t_start);
        }
    }
    if (facade.store().block_insert(block) != 0) {
        facade.write_coordinator().Rollback(txn);
        return finalizer_.Fail(task, "CX_ERR_F41_WRITE_FAILED",
                               "doc_summary block_insert failed",
                               {{"doc_id", task.doc_id}}, t_start);
    }
    Status cm = facade.write_coordinator().Commit(txn);
    if (!cm.ok())
        return finalizer_.Fail(task, "CX_ERR_F41_WRITE_FAILED", cm.message(),
                               {{"doc_id", task.doc_id}}, t_start);

    return finalizer_.Complete(task, task.doc_id, t_start);
}

}  // namespace doc_summary
}  // namespace cortrix
