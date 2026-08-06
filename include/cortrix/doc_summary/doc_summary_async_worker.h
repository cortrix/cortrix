#pragma once
#include <memory>

#include "cortrix/async/task_finalizer.h"
#include "cortrix/async/task_handler.h"
#include "cortrix/async/task_info.h"
#include "cortrix/common/status.h"
#include "cortrix/doc_summary/doc_summary_generator.h"  // DocSummaryConfig + DocSummaryGenerator + GenerationResult
#include "cortrix/llm/i_llm_client.h"

namespace cortrix {

class OnnxEmbedder;
class BlockAssembler;
namespace resource { class INamespacePool; }

namespace doc_summary {

/// DocSummaryAsyncWorker — the async-task callback for kTaskDocSummary.
/// For one document it:
///   (1) acquires the task's NamespaceFacade from the namespace pool (RAII Release),
///       exactly as SPCManager does before SPCPipeline::Process;
///   (2) builds a PER-TASK SqliteChunkStore over THAT Unit's blocks + a
///       DocSummaryGenerator, and runs the LLM map-reduce summary (DocSummaryGenerator
///       binds one ChunkStore; the worker serves many namespaces → per-task build);
///   (3) re-embeds the summary_text (OnnxEmbedder — Generate leaves embedding empty);
///   (4) assembles a doc_summary Block (block_type=17, metadata_json status="generated")
///       and writes it in one WriteCoordinator txn (BeginWrite → AddPoints[P-HNSW]
///       → block_insert → Commit), mirroring the SPCPipeline memory/L1 write path.
///
/// On any failure it returns the error Status WITHOUT writing a block — the scheduler owns the
/// retry / DLQ (3 attempts), and doc-discovery degrades to the metadata-field FTS5 fallback
/// for that doc (a failed doc simply has no doc_summary block). The doc-level
/// FTS5 index (rule-extracted fields, not the LLM summary) is a separate path; wiring it to
/// the per-Unit store is its own integration item, NOT this worker.
///
/// ProcessTask(TaskInfo) is shaped like async::DocumentProcessor::ProcessTask so the
/// WorkerPool can dispatch by task_type (the dispatch wiring itself is scheduler productization).
///
/// Metrics: the doc_summary OBS counters (llm_calls_total / summaries_generated_total)
/// are recorded by DocSummaryGenerator itself (the LLM data layer); the worker does
/// NOT re-record them (no double counting). It is a pure orchestration layer.
///
/// Seams: the LLM is the frozen llm::ILlmClient (prod OpenAiLlmClient; tests
/// MockLlmClient); chunks come from SqliteChunkStore over the acquired Unit store;
/// embeddings from OnnxEmbedder; the doc_summary block via the façade's PWL.
class DocSummaryAsyncWorker : public async::ITaskHandler {
public:
    /// @param pool       borrowed namespace pool (per-task Acquire/Release).
    /// @param llm_client shared LLM client passed into each per-task generator.
    /// @param config     resolved DocSummaryConfig (prompt/model/threshold).
    /// @param embedder   borrowed OnnxEmbedder for the summary_text re-embed.
    /// @param assembler  borrowed BlockAssembler (flat Assemble → doc_summary block).
    /// @param task_mgr   borrowed TaskManager — finalizes the task terminal state via
    ///                    TaskFinalizer (success → MarkCompleted; any failure → MarkFailed
    ///                    with the CX_ERR_DOCSUMMARY_* code + structured_data).
    DocSummaryAsyncWorker(resource::INamespacePool& pool,
                   std::shared_ptr<llm::ILlmClient> llm_client,
                   DocSummaryConfig config,
                   OnnxEmbedder& embedder,
                   BlockAssembler& assembler,
                   async::TaskManager* task_mgr);

    /// Process one doc_summary task end-to-end (see class comment). Returns Ok on a
    /// persisted doc_summary block; an error Status (façade Acquire / generation /
    /// embed / write failure, carrying the CX_ERR_* identity) otherwise — the
    /// caller (the worker) maps it to retry / DLQ.
    Status ProcessTask(const async::TaskInfo& task) override;

private:
    resource::INamespacePool& pool_;
    std::shared_ptr<llm::ILlmClient> llm_client_;
    DocSummaryConfig config_;
    OnnxEmbedder& embedder_;
    BlockAssembler& assembler_;
    async::TaskFinalizer finalizer_;  // terminal write + task metric
};

}  // namespace doc_summary
}  // namespace cortrix
