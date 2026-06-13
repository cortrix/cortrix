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

// D3.5 wire⑤c: live doc/block counts come from a per-request NamespaceFacade over
// the F05 resource pool; namespace creation is routed through the F12 catalog.
namespace resource { class INamespacePool; }
namespace catalog  { class INSRouter; }

class CortrixHttpServer {
public:
    CortrixHttpServer(const CortrixConfig& config,
                     ApiKeyAuth& auth,
                     NamespaceManager& ns_mgr);

    /// Register all routes (F01 routes + extension points for later Features)
    void RegisterRoutes();

    /// Blocking start
    /// @return Ok / Internal(bind failure)
    Status Start();

    /// Graceful shutdown
    void Stop();

    /// Set the F05 NS resource pool for live doc/block count queries (per-request
    /// NamespaceFacade Acquire/Release). Replaces the MVP CortrixNamespaceManager.
    void SetNamespacePool(cortrix::resource::INamespacePool* pool);

    /// Set the F12 NS router used by the create-namespace route (F13). When unset,
    /// POST /api/v1/namespaces falls back to the metadata manager (NamespaceManager).
    void SetNamespaceRouter(cortrix::catalog::INSRouter* router);

    /// Serve the P02a web UI (SPA) from `dir` at the server root. Static assets
    /// are mounted at "/"; unmatched non-/api GET paths fall back to index.html
    /// (BrowserRouter deep links). No-op if dir/index.html is unreadable.
    void EnableWebUi(const std::string& dir);

    /// Get underlying httplib::Server reference (for later Feature route registration)
    httplib::Server& server();

private:
    void RegisterHealthRoutes();
    void RegisterSystemRoutes();
    void RegisterNamespaceRoutes();

    // F12 single-SoT accessors: read/delete through the catalog router when
    // injected (creation already does), legacy NamespaceManager otherwise.
    nlohmann::json BuildNamespaceListJson(const std::vector<std::string>& allowed);
    Status BuildNamespaceJson(const std::string& name, nlohmann::json* out);
    Status DeleteNamespaceUnified(const std::string& name);

    CortrixConfig config_;
    ApiKeyAuth& auth_;
    NamespaceManager& ns_mgr_;
    cortrix::resource::INamespacePool* pool_ = nullptr;
    cortrix::catalog::INSRouter* ns_router_ = nullptr;
    httplib::Server svr_;
    std::string web_ui_index_;  // index.html payload for the SPA 404 fallback
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point start_time_;
};

/// Write JSON error response
void WriteJsonError(httplib::Response& res, const Status& status,
                    const std::string& request_id = "");
void WriteJsonResponse(httplib::Response& res, int http_status,
                       const nlohmann::json& body,
                       const std::string& request_id = "");

}  // namespace cortrix
