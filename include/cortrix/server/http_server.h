#pragma once
#include <memory>
#include <atomic>
#include <chrono>
#include "httplib.h"
#include <nlohmann/json.hpp>
#include "cortrix/config/config.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/namespace/namespace_manager.h"

namespace cortrix {

// integration wire⑤c: live doc/block counts come from a per-request NamespaceFacade over
// the resource pool; namespace creation is routed through the catalog.
namespace resource {
class INamespacePool;
}
namespace catalog {
class INSRouter;
}
namespace observability {
class IOperationLogger;
}

class CortrixHttpServer {
public:
    CortrixHttpServer(const CortrixConfig& config, ApiKeyAuth& auth, NamespaceManager& ns_mgr);

    /// Register all routes (index routes + extension points)
    void RegisterRoutes();

    /// Blocking start
    /// @return Ok / Internal(bind failure)
    Status Start();

    /// Graceful shutdown
    void Stop();

    /// Set the NS resource pool for live doc/block count queries (per-request
    /// NamespaceFacade Acquire/Release). Replaces the MVP CortrixNamespaceManager.
    void SetNamespacePool(cortrix::resource::INamespacePool* pool);

    /// Set the NS router used by the create-namespace route. When unset,
    /// POST /api/v1/namespaces falls back to the metadata manager (NamespaceManager).
    void SetNamespaceRouter(cortrix::catalog::INSRouter* router);

    /// Set the operation_log writer for the NamespaceManager instrumentation
    /// site (ns_create / ns_delete). Optional — when unset the NS routes run
    /// unchanged (observability strictly additive, C4). Non-owning: the logger
    /// outlives the server (bootstrap owns the ObservabilityModule).
    void SetOperationLogger(cortrix::observability::IOperationLogger* op_logger);

    /// Open-Core edition seam (ARCH OPEN-6). The stock CE binary leaves
    /// this at the default "ce". An enterprise embedder sets "enterprise" from its
    /// on_assembled hook, so GET /api/v1/system/features advertises the enterprise
    /// edition; the web UI gates the Enterprise nav group on this value. Must be
    /// called before Start() — on_assembled runs before the listener, so the value
    /// is in place by the time any request reads it (no concurrent write/read).
    void SetEdition(const std::string& edition);

    /// Serve the web UI (SPA) from `dir` at the server root. Static assets
    /// are mounted at "/"; unmatched non-/api GET paths fall back to index.html
    /// (BrowserRouter deep links). No-op if dir/index.html is unreadable.
    void EnableWebUi(const std::string& dir);

    /// Get underlying httplib::Server reference (for later Feature route registration)
    httplib::Server& server();

private:
    void RegisterHealthRoutes();
    void RegisterOpenApiRoutes();
    void RegisterSystemRoutes();
    void RegisterNamespaceRoutes();

    // Single-SoT accessors: read/delete through the catalog router when
    // injected (creation already does), legacy NamespaceManager otherwise.
    // `filter`=false lists every namespace (no-auth dev mode). `filter`=true
    // restricts the result to `allowed` (the principal's authorized set from
    // PermissionService::ListAuthorizedNamespaces); an empty `allowed` under
    // filter mode lists NONE (deny-by-default), not all.
    nlohmann::json BuildNamespaceListJson(const std::vector<std::string>& allowed, bool filter);
    Status BuildNamespaceJson(const std::string& name, nlohmann::json* out);
    Status DeleteNamespaceUnified(const std::string& name);

    // Emit one operation_log row (action
    // ns_create / ns_delete) on a successful NS mutation. No-op when op_logger_ is
    // null. Identity is read from the thread-local ObservabilityContext (the route
    // runs synchronously on the request thread WithAuth populated). Never throws
    // across the request path (the logger is no-throw).
    void EmitNsLog(const std::string& action, const std::string& name);

    CortrixConfig config_;
    ApiKeyAuth& auth_;
    NamespaceManager& ns_mgr_;
    cortrix::resource::INamespacePool* pool_ = nullptr;
    cortrix::catalog::INSRouter* ns_router_ = nullptr;
    cortrix::observability::IOperationLogger* op_logger_ = nullptr;
    httplib::Server svr_;
    std::string edition_ = "ce";  // Open-Core edition reported by GET /system/features
    std::string web_ui_index_;  // index.html payload for the SPA 404 fallback
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point start_time_;
};

/// Write JSON error response
void WriteJsonError(httplib::Response& res, const Status& status,
                    const std::string& request_id = "");
void WriteJsonResponse(httplib::Response& res, int http_status, const nlohmann::json& body,
                       const std::string& request_id = "");

}  // namespace cortrix
