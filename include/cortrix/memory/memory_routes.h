#pragma once
#include <memory>
#include <string>

namespace httplib { class Server; }

namespace cortrix {

namespace resource { class INamespacePool; }
namespace observability { class IOperationLogger; }  // D3.5 wire⑤c: memory routes acquire per-request façades from the pool

class ApiKeyAuth;
class SPCManager;
class OnnxEmbedder;
class IntentClassifier;
class RRFFusion;
struct MemoryConfig;

namespace memory {
class MemoryExtractionService;          // M1/M2 — /memory/extract* + interaction enqueue
}  // namespace memory

/// M-wave (D3.5 round-2) memory service bundle. Threads the MEM02 runtime service into
/// RegisterMemoryRoutes without a long parameter list. The pointer is owned by bootstrap
/// and outlives the server; null = extraction disabled (the extract endpoints then
/// return a clean disabled error). MEM03 transparency + MEM04 opt-out are NS-scoped, so
/// they are built per-request over the façade (not held here).
struct MemoryServices {
    memory::MemoryExtractionService* extraction = nullptr;   ///< M1/M2 extract + enqueue
    /// F18a audit sink for the MEM03 transparency surface (memory_create/edit/
    /// invalidate op-log entries). Null = audit disabled (handlers stay safe).
    std::shared_ptr<observability::IOperationLogger> op_logger;
};

/// Register all Memory API endpoints
///
/// Sessions:
///   POST   /api/v1/memory/sessions
///   GET    /api/v1/memory/sessions
///   GET    /api/v1/memory/sessions/:session_id
///   DELETE /api/v1/memory/sessions/:session_id
///
/// Interactions:
///   POST   /api/v1/memory/sessions/:session_id/interactions
///
/// Search:
///   POST   /api/v1/memory/search
///
/// Inject:
///   POST   /api/v1/memory/inject
void RegisterMemoryRoutes(
    httplib::Server& svr,
    ApiKeyAuth& auth,
    resource::INamespacePool& pool,
    SPCManager& spc_mgr,
    OnnxEmbedder& embedder,
    IntentClassifier& classifier,
    RRFFusion& fusion,
    const MemoryConfig& config,
    const MemoryServices& services = {}
);

}  // namespace cortrix
