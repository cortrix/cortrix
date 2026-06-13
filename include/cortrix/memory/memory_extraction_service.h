#pragma once
#include <memory>
#include <string>

#include "cortrix/memory/interaction_log.h"
#include "cortrix/memory/memory_extractor.h"
#include "cortrix/memory/memory_queue.h"

namespace cortrix {
class OnnxEmbedder;
namespace llm { class ILlmClient; }
namespace resource { class INamespacePool; }
namespace observability { class IOperationLogger; }
namespace memory::immunity { class OptOutManager; }
}  // namespace cortrix

namespace cortrix::memory {

// M1 (D3.5 round-2) — the runtime assembly of MEM02: a MemoryQueue draining
// interactions through per-namespace MemoryExtractors. The interaction-write route
// enqueues each interaction; the worker pool drains them. Per item the worker
// acquires the interaction's namespace façade and builds the NS-scoped MemoryExtractor
// (real MemoryBlockAdapter + MemoryContradictionAdapter over that NS's store), so
// memories land in the right namespace's blocks table.
//
// MEM04 double-check (the §3.3.1 worker logic, kernel-side here instead of the design's
// Python middleware): before extracting, the worker (1) skips when the session is
// opted out (records operation_log.memory_extract_skipped) and (2) skips +
// session-opts-out when interaction.remember==false (D2). When no LLM is configured the
// service is OFF — interactions are still written (interaction_log) but never extracted.
class MemoryExtractionService {
public:
    /// @param pool       the namespace pool (per-item façade acquisition).
    /// @param llm        shared extraction LLM (null → service disabled, log-only).
    /// @param embedder   embeds memory content for search (shared with the pipeline).
    /// @param op_logger  F18a operation logger (memory_extract / _skipped audit).
    /// @param config     mem02.* config.
    /// @param queue_cfg  worker pool + reliability config (worker_count etc).
    ///
    /// The MEM04 opt-out check is built per-item over the interaction's NS façade
    /// (sessions are NS-scoped), so no global opt-out manager is held here.
    MemoryExtractionService(resource::INamespacePool& pool,
                            std::shared_ptr<llm::ILlmClient> llm,
                            OnnxEmbedder& embedder,
                            std::shared_ptr<observability::IOperationLogger> op_logger,
                            MemoryExtractorConfig config,
                            MemoryQueue::Config queue_cfg);
    ~MemoryExtractionService();

    MemoryExtractionService(const MemoryExtractionService&) = delete;
    MemoryExtractionService& operator=(const MemoryExtractionService&) = delete;

    /// True when an LLM is configured (extraction actually runs). When false, Enqueue
    /// is a no-op and the route degrades to interaction_log only.
    bool enabled() const { return llm_ != nullptr; }

    /// Start the worker pool (idempotent). No-op when disabled.
    void Start();

    /// Stop + join workers (idempotent; also run by the dtor).
    void Stop();

    /// Enqueue an interaction for async extraction (called by the interaction-write
    /// route after a successful write). Returns false on backpressure / disabled.
    bool Enqueue(const InteractionLog& interaction);

    /// The MemoryQueue (for depth metrics / tests).
    MemoryQueue& queue() { return queue_; }

    /// Run extraction for one interaction synchronously (the worker handler + the
    /// /memory/extract HTTP path share this). Acquires the NS façade, applies the
    /// MEM04 double-check, builds the NS-scoped extractor, and runs it. Returns the
    /// extractor result (or a non-ok result carrying the failure / skip reason).
    MemoryExtractionResult ExtractOne(const InteractionLog& interaction,
                                      const observability::TraceContext* ctx = nullptr);

private:
    bool HandleItem(const MemoryQueueItem& item);

    resource::INamespacePool& pool_;
    std::shared_ptr<llm::ILlmClient> llm_;
    OnnxEmbedder& embedder_;
    std::shared_ptr<observability::IOperationLogger> op_logger_;
    MemoryExtractorConfig config_;
    MemoryQueue queue_;
    bool started_ = false;
};

}  // namespace cortrix::memory
