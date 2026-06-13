#include "cortrix/memory/memory_writer.h"
#include "cortrix/store/cortrix_store.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>

namespace cortrix {

MemoryWriter::MemoryWriter(MemoryStore& memory_store,
                           SPCManager& spc_mgr,
                           CortrixStore& store,
                           const MemoryConfig& config)
    : memory_store_(memory_store)
    , spc_mgr_(spc_mgr)
    , store_(store)
    , config_(config) {}

Status MemoryWriter::Write(const MemoryWriteRequest& request) {
    // 1. Parameter validation
    if (request.session_id.empty()) {
        return Status::InvalidArgument("session_id is required");
    }
    if (request.query_text.empty()) {
        return Status::InvalidArgument("query_text is required");
    }
    if (request.response_text.empty()) {
        return Status::InvalidArgument("response_text is required");
    }

    // 2. Verify session exists
    MemorySession session;
    auto s = memory_store_.SessionGet(request.session_id, session);
    if (!s.ok()) return s;

    // 3a. Build user interaction log
    InteractionLog user_log;
    user_log.session_id = request.session_id;
    user_log.namespace_name = request.namespace_name;
    user_log.user_id = request.user_id;
    user_log.role = "user";
    user_log.content = request.query_text;
    user_log.query_type = request.query_type;

    // 3b. Build assistant interaction log
    InteractionLog assistant_log;
    assistant_log.session_id = request.session_id;
    assistant_log.namespace_name = request.namespace_name;
    assistant_log.user_id = request.user_id;
    assistant_log.role = "assistant";
    assistant_log.content = request.response_text;
    assistant_log.status = request.response_status;
    assistant_log.latency_ms = request.latency_ms;
    assistant_log.metadata_json = request.metadata_json;

    // 3c. Atomically insert both interactions + touch session under a single mutex lock.
    // This fixes C-001: previously BEGIN TRANSACTION was issued without holding mu_,
    // allowing concurrent threads to interleave SQL within the transaction window.
    s = memory_store_.InteractionPairInsertAndSessionTouch(
            user_log, assistant_log, request.session_id);
    if (!s.ok()) return s;

    // 3d. Find or create session document (separate store, not part of memory_store_ transaction)
    std::string doc_id;
    s = FindOrCreateSessionDoc(request.session_id, request.namespace_name, &doc_id);
    if (!s.ok()) {
        // Non-fatal: SPC enqueue below will simply not have a valid doc_id
        doc_id.clear();
    }

    // 4. SPC enqueue (best-effort, after commit)
    std::string content = BuildMemoryContent(request.query_text, request.response_text);

    // Determine TTL
    int ttl = request.ttl_seconds;
    if (ttl == 0 && request.result_source == "text_to_sql") {
        ttl = config_.text_to_sql_ttl_seconds;
    }

    // Get turn number from session
    int64_t count = 0;
    memory_store_.InteractionCount(request.session_id, &count);
    int turn = static_cast<int>(count / 2);  // each turn = 2 interactions

    MemoryWriteRequest req_with_ttl = request;
    req_with_ttl.ttl_seconds = ttl;

    std::string metadata = BuildMemoryMetadata(req_with_ttl, user_log.id, turn);

    auto task = CreateSPCTask(request.session_id, request.namespace_name, doc_id, content, metadata);
    if (task) {
        task->metadata_json = metadata;  // Flow metadata through SPC pipeline to block storage
        auto submit_status = spc_mgr_.Submit(task);
        if (!submit_status.ok()) {
            spdlog::warn("SPC enqueue failed for session {}: {}",
                         request.session_id, submit_status.message());
        }
    }

    return Status::Ok();
}

Status MemoryWriter::FindOrCreateSessionDoc(const std::string& session_id,
                                             const std::string& /*namespace_name*/,
                                             std::string* doc_id) {
    CortrixDoc doc;
    int rc = store_.doc_find_by_source("memory_session", session_id, doc);

    if (rc == 0) {
        // Document exists, reuse
        *doc_id = doc.doc_id;
        return Status::Ok();
    }

    // Create new document
    CortrixDoc new_doc;
    new_doc.source_type = "memory_session";
    new_doc.source_path = session_id;
    new_doc.status = DocStatus::kReady;
    new_doc.title = "Memory Session: " + session_id;
    new_doc.chunk_strategy = "per_turn";

    rc = store_.doc_create(new_doc);
    if (rc != 0) {
        return Status::Internal("Failed to create session document");
    }

    *doc_id = new_doc.doc_id;
    return Status::Ok();
}

std::string MemoryWriter::BuildMemoryContent(const std::string& query_text,
                                              const std::string& response_text) {
    return query_text + "\n---\n" + response_text;
}

std::string MemoryWriter::BuildMemoryMetadata(const MemoryWriteRequest& request,
                                               const std::string& interaction_id,
                                               int turn_number) {
    nlohmann::json meta;
    meta["interaction_id"] = interaction_id;
    meta["turn"] = turn_number;
    meta["query_type"] = request.query_type;
    meta["result_source"] = request.result_source;
    meta["session_id"] = request.session_id;
    meta["user_id"] = request.user_id;

    // Per design spec (6.5.2): queried_at and ttl_seconds only for text_to_sql type.
    // document_query and conversation types have infinite TTL (no expiry).
    if (request.result_source == "text_to_sql") {
        meta["ttl_seconds"] = request.ttl_seconds;

        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
        gmtime_r(&time_t_now, &tm_buf);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
            tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
            static_cast<int>(ms % 1000));
        meta["queried_at"] = std::string(buf);
    }

    return meta.dump();
}

std::shared_ptr<SPCTask> MemoryWriter::CreateSPCTask(const std::string& session_id,
                                                      const std::string& namespace_name,
                                                      const std::string& doc_id,
                                                      const std::string& content,
                                                      const std::string& metadata_json) {
    auto task = std::make_shared<SPCTask>();
    task->source_type = "memory_session";
    task->source_path = session_id;
    task->namespace_name = namespace_name;
    task->doc_id = doc_id;
    task->priority = SPCPriority::kP0;
    task->stage = SPCStage::kQueued;
    // Pass content and metadata for SPC pipeline to process
    // content_hash used to carry the Q+A text for the memory block
    task->content_hash = content;
    // mime_type field repurposed to carry metadata_json for memory blocks
    task->mime_type = metadata_json;
    return task;
}

}  // namespace cortrix
