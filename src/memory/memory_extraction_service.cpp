#include "cortrix/memory/memory_extraction_service.h"

#include "cortrix/llm/i_llm_client.h"
#include "cortrix/logging/logging.h"
#include "cortrix/memory/memory_block_adapter.h"
#include "cortrix/memory/opt_out_manager.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/resource/namespace_pool.h"

namespace cortrix::memory {

MemoryExtractionService::MemoryExtractionService(
    resource::INamespacePool& pool, std::shared_ptr<llm::ILlmClient> llm,
    OnnxEmbedder& embedder, std::shared_ptr<observability::IOperationLogger> op_logger,
    MemoryExtractorConfig config, MemoryQueue::Config queue_cfg)
    : pool_(pool),
      llm_(std::move(llm)),
      embedder_(embedder),
      op_logger_(std::move(op_logger)),
      config_(std::move(config)),
      queue_(queue_cfg) {}

MemoryExtractionService::~MemoryExtractionService() { Stop(); }

void MemoryExtractionService::Start() {
    if (started_ || !enabled()) return;
    queue_.StartWorkers([this](const MemoryQueueItem& item) { return HandleItem(item); });
    started_ = true;
    CORTRIX_LOG_INFO("mem02", "memory extraction workers started ({} workers)",
                     queue_.worker_count());
}

void MemoryExtractionService::Stop() {
    if (!started_) return;
    queue_.Stop();
    started_ = false;
}

bool MemoryExtractionService::Enqueue(const InteractionLog& interaction) {
    if (!enabled()) return false;
    return queue_.Push(interaction);
}

bool MemoryExtractionService::HandleItem(const MemoryQueueItem& item) {
    auto r = ExtractOne(item.interaction, item.has_ctx ? &item.ctx : nullptr);
    // A skip (opt-out / remember=false / disabled) and a success both "consume" the
    // item; only a hard extractor failure routes to retry/dead-letter.
    return r.ok();
}

MemoryExtractionResult MemoryExtractionService::ExtractOne(
    const InteractionLog& interaction, const observability::TraceContext* ctx) {
    MemoryExtractionResult result;
    if (!enabled()) {
        // Service disabled (no LLM) — interaction_log already written upstream; nothing
        // to extract. Treat as a benign success (no retry).
        return result;
    }

    const std::string& ns = interaction.namespace_name;

    // Acquire the interaction's namespace façade first (sessions / blocks / opt-out are
    // all NS-scoped). A vanished / unavailable namespace is not retryable.
    resource::NamespaceFacade facade(pool_, ns);
    if (Status acq = facade.Acquire(); !acq.ok()) {
        CORTRIX_LOG_WARN("mem02",
            "extraction skipped — namespace '{}' not available: {}", ns, acq.message());
        return result;  // ok() == true → no retry
    }

    auto skip_oplog = [&](const char* why) {
        if (!op_logger_) return;
        observability::OperationLogEntry e;
        e.user_id = interaction.user_id.empty() ? "anonymous" : interaction.user_id;
        e.action = "memory_extract_skipped";
        e.namespace_id = ns;
        e.resource_type = "memory";
        e.resource_id = interaction.session_id;
        e.summary = std::string("Skipped extraction (") + why + ")";
        op_logger_->Log(e, ctx);
    };

    // MEM04 opt-out manager over this NS's session store (NS-scoped).
    immunity::MemoryStoreOptOutAdapter opt_store(facade.memory());
    immunity::OptOutManager opt_out(
        std::shared_ptr<immunity::ISessionOptOutStore>(&opt_store, [](auto*) {}),
        op_logger_, /*enabled=*/true);

    // MEM04 double-check 2 (D2): interaction.remember==false → skip + session opt-out.
    if (!interaction.remember) {
        opt_out.OptOut(interaction.session_id, immunity::OptOutActor::kSystemAuto,
                       "interaction_field_remember_false", ctx);
        skip_oplog("interaction remember=false");
        return result;  // skip
    }

    // MEM04 double-check 1 (D3): session opted out → skip + record memory_extract_skipped.
    if (auto opted = opt_out.is_session_opted_out(interaction.session_id, ctx);
        opted.ok() && opted.value()) {
        skip_oplog("session opted out");
        return result;  // skip
    }

    auto block_store = std::make_shared<MemoryBlockAdapter>(
        facade.store(), &embedder_, &facade.vec_index());
    auto contradiction = std::make_shared<MemoryContradictionAdapter>(facade.store());

    MemoryExtractor extractor(llm_, block_store, contradiction, op_logger_, config_);
    // Single-turn window (the current interaction). The multi-turn window fetch from
    // the session store is a Phase-1.x refinement (MEM02 §3.2 get_window); a single
    // turn is the minimal correct extraction unit.
    result = extractor.Extract(interaction, ctx);
    if (!result.ok()) {
        CORTRIX_LOG_WARN("mem02", "extraction failed for interaction {}: {}",
                         interaction.id, result.error->message);
    }
    return result;
}

}  // namespace cortrix::memory
