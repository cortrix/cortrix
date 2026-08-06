#pragma once
#include <memory>
#include <string>
#include "cortrix/common/status.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/spc_task.h"

namespace cortrix {

class CortrixStore;

/// Single-turn interaction write request (one Q+A pair)
struct MemoryWriteRequest {
    std::string session_id;
    std::string namespace_name;
    std::string user_id;

    // User input
    std::string query_text;
    std::string query_type;            // "semantic" | "sql" | "hybrid" | "chat"

    // System response
    std::string response_text;
    std::string response_status;       // "success" | "error" | "partial"
    int latency_ms = 0;

    // Memory attributes
    std::string result_source;         // "document_query" | "text_to_sql" | "conversation"
    int ttl_seconds = 0;              // TTL (0 = never expires)
    std::string metadata_json;
};

class MemoryWriter {
public:
    /// @param memory_store: MemoryStore instance (per-namespace)
    /// @param spc_mgr: SPCManager instance (global shared)
    /// @param store: per-namespace CortrixStore view (integration: the caller holds a
    ///        NamespaceFacade for the request and passes facade.store(); the
    ///        writer only needs the metadata store for the session document)
    /// @param config: MemoryConfig
    MemoryWriter(MemoryStore& memory_store,
                 SPCManager& spc_mgr,
                 CortrixStore& store,
                 const MemoryConfig& config);

    /// Write one interaction round (Q+A dual-write)
    ///
    /// Flow:
    /// 1. Same SQLite transaction:
    ///    a. INSERT interaction_log (user role, query_text)
    ///    b. INSERT interaction_log (assistant role, response_text)
    ///    c. Find or create Document (source_type=memory_session)
    ///    d. Session touch (update updated_at + interaction_count)
    /// 2. After transaction commit:
    ///    e. Create SPCTask (source_type=memory_session) -> SPCManager.Submit()
    ///
    /// @param request: write request (contains Q+A pair)
    /// @return Ok / NotFound(session not found) / Internal
    Status Write(const MemoryWriteRequest& request);

private:
    Status FindOrCreateSessionDoc(const std::string& session_id,
                                  const std::string& namespace_name,
                                  std::string* doc_id);

    static std::string BuildMemoryContent(const std::string& query_text,
                                          const std::string& response_text);

    static std::string BuildMemoryMetadata(const MemoryWriteRequest& request,
                                           const std::string& interaction_id,
                                           int turn_number);

    std::shared_ptr<SPCTask> CreateSPCTask(const std::string& session_id,
                                            const std::string& namespace_name,
                                            const std::string& doc_id,
                                            const std::string& content,
                                            const std::string& metadata_json);

    MemoryStore& memory_store_;
    SPCManager& spc_mgr_;
    CortrixStore& store_;
    MemoryConfig config_;
};

}  // namespace cortrix
