#include <cstdint>
#include "cortrix/server/http_server.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/catalog/i_ns_router.h"          // F12 INSRouter (F13 create path)
#include "cortrix/catalog/catalog_types.h"        // NSMetadata
#include "cortrix/resource/namespace_facade.h"    // per-request façade over F05 pool
#include "cortrix/logging/logging.h"
#include "cortrix/common/version.h"                // [P5] version SoT

#include <random>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <ctime>

#include <openssl/rand.h>

namespace cortrix {

// GenerateRequestId: 16 random bytes -> 32-char hex string
std::string GenerateRequestId() {
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        ss << std::setw(2) << static_cast<int>(buf[i]);
    }
    return ss.str();
}

void WriteJsonError(httplib::Response& res, const Status& status,
                    const std::string& request_id) {
    nlohmann::json body;
    body["error"]["code"] = status.error_code_string();
    body["error"]["message"] = status.message();
    if (!request_id.empty()) {
        body["error"]["request_id"] = request_id;
        res.set_header("X-Request-Id", request_id);
    }
    // Per API_GATEWAY_DESIGN.md section 2.5.1: include ISO 8601 timestamp
    {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
        gmtime_r(&time_t_now, &tm_buf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
        std::ostringstream ts;
        ts << buf << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
        body["error"]["timestamp"] = ts.str();
    }
    res.set_content(body.dump(), "application/json");
    res.status = status.http_status();
}

void WriteJsonResponse(httplib::Response& res, int http_status,
                       const nlohmann::json& body,
                       const std::string& request_id) {
    if (!request_id.empty()) {
        res.set_header("X-Request-Id", request_id);
    }
    res.set_content(body.dump(), "application/json");
    res.status = http_status;
}

CortrixHttpServer::CortrixHttpServer(const CortrixConfig& config,
                                   ApiKeyAuth& auth,
                                   NamespaceManager& ns_mgr)
    : config_(config), auth_(auth), ns_mgr_(ns_mgr) {}

void CortrixHttpServer::RegisterRoutes() {
    RegisterHealthRoutes();
    RegisterSystemRoutes();
    RegisterNamespaceRoutes();

    // 404 handler for unmatched routes
    // Uses WriteJsonError() to ensure consistent error format with timestamp
    // per API_GATEWAY_DESIGN.md section 2.5.1. When the web UI is enabled,
    // unmatched non-/api GETs serve index.html instead (SPA deep links).
    svr_.set_error_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (res.status == 404) {
            if (!web_ui_index_.empty() && req.method == "GET" &&
                req.path.rfind("/api/", 0) != 0) {
                res.status = 200;
                res.set_content(web_ui_index_, "text/html");
                return;
            }
            std::string req_id = GenerateRequestId();
            WriteJsonError(res, Status::NotFound("The requested resource was not found"), req_id);
        }
    });
}

void CortrixHttpServer::EnableWebUi(const std::string& dir) {
    std::ifstream index(dir + "/index.html", std::ios::binary);
    if (!index) {
        CORTRIX_LOG_WARN("server", "Web UI dir '{}' has no readable index.html — UI disabled", dir);
        return;
    }
    std::stringstream ss;
    ss << index.rdbuf();
    web_ui_index_ = ss.str();
    svr_.set_mount_point("/", dir);
    CORTRIX_LOG_INFO("server", "Web UI enabled from {}", dir);
}

Status CortrixHttpServer::Start() {
    start_time_ = std::chrono::steady_clock::now();
    running_ = true;

    svr_.new_task_queue = [this] {
        return new httplib::ThreadPool(config_.server.thread_count);
    };

    CORTRIX_LOG_INFO("server", "Starting HTTP server on {}:{}", config_.server.host, config_.server.port);

    if (!svr_.listen(config_.server.host, config_.server.port)) {
        running_ = false;
        return Status::Internal("Failed to bind to " + config_.server.host + ":" + std::to_string(config_.server.port));
    }

    return Status::Ok();
}

void CortrixHttpServer::Stop() {
    if (running_) {
        running_ = false;
        svr_.stop();
        CORTRIX_LOG_INFO("server", "HTTP server stopped");
    }
}

void CortrixHttpServer::SetNamespacePool(cortrix::resource::INamespacePool* pool) {
    pool_ = pool;
}

void CortrixHttpServer::SetNamespaceRouter(cortrix::catalog::INSRouter* router) {
    ns_router_ = router;
}

httplib::Server& CortrixHttpServer::server() {
    return svr_;
}

void CortrixHttpServer::RegisterHealthRoutes() {
    svr_.Get("/api/v1/health", NoAuth(
        [this](const httplib::Request&, httplib::Response& res, const RequestContext& rctx) {
            auto now = std::chrono::steady_clock::now();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();

            nlohmann::json body;
            body["status"] = "healthy";
            body["version"] = cortrix::kCortrixVersion;
            body["uptime_seconds"] = uptime;
            body["components"]["config"] = "ok";
            body["components"]["logging"] = "ok";
            body["components"]["namespace_manager"] = "ok";

            body["llm_enabled"] = config_.semantic_llm.IsConfigured();
            body["llm_provider"] = config_.semantic_llm.provider;
            body["llm_model"] = config_.semantic_llm.model;

            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));
}

void CortrixHttpServer::RegisterSystemRoutes() {
    // GET /api/v1/system/features
    // Returns the edition plus which optional features this build provides.
    // Stable schema (Agent-friendly principle 7): unavailable features report
    // false rather than disappearing from the body.
    svr_.Get("/api/v1/system/features", NoAuth(
        [](const httplib::Request&, httplib::Response& res, const RequestContext& rctx) {
            nlohmann::json body;
            body["edition"] = "ce";
            body["features"]["cdc"]          = false;
            body["features"]["text_to_sql"]  = false;
            // Phase 2+ features (not yet implemented)
            body["features"]["fuse"]     = false;
            body["features"]["s3_proxy"] = false;

            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));
}

nlohmann::json CortrixHttpServer::BuildNamespaceListJson(
    const std::vector<std::string>& allowed) {
    nlohmann::json resp;
    resp["namespaces"] = nlohmann::json::array();
    auto append = [&](const std::string& name, int64_t created_at, int64_t updated_at,
                      int64_t dc, int64_t bc) {
        // Live counts from the store when the NS is resident in the pool
        // (per-request façade); metadata counts otherwise.
        if (pool_) {
            cortrix::resource::NamespaceFacade facade(*pool_, name);
            if (facade.Acquire().ok()) {
                facade.store().doc_count(&dc);
                facade.store().block_count(&bc);
            }
        }
        nlohmann::json j;
        j["name"] = name;
        j["created_at"] = created_at;
        j["updated_at"] = updated_at;
        j["doc_count"] = dc;
        j["block_count"] = bc;
        resp["namespaces"].push_back(j);
    };

    if (ns_router_) {
        auto listed = ns_router_->ListNamespaces({});
        if (listed.ok()) {
            for (const auto& ns_id : listed.value().results) {
                if (!allowed.empty() &&
                    std::find(allowed.begin(), allowed.end(), ns_id) == allowed.end()) {
                    continue;
                }
                // NSMetadata has no updated_at column; mirror the create
                // response and reuse created_at.
                int64_t created = 0;
                auto got = ns_router_->GetNamespace(ns_id);
                if (got.ok()) created = got.value().created_at;
                append(ns_id, created, created, 0, 0);
            }
            resp["total"] = resp["namespaces"].size();
            return resp;
        }
    }

    auto nss = ns_mgr_.List(allowed);
    for (const auto& ns : nss) {
        append(ns.name, ns.created_at, ns.updated_at, ns.doc_count, ns.block_count);
    }
    resp["total"] = resp["namespaces"].size();
    return resp;
}

Status CortrixHttpServer::BuildNamespaceJson(const std::string& name,
                                             nlohmann::json* out) {
    int64_t created = 0, updated = 0, dc = 0, bc = 0;
    if (ns_router_) {
        auto got = ns_router_->GetNamespace(name);
        if (!got.ok()) return got.status();
        created = got.value().created_at;
        updated = created;
    } else {
        NamespaceInfo info;
        Status s = ns_mgr_.Get(name, &info);
        if (!s.ok()) return s;
        created = info.created_at;
        updated = info.updated_at;
        dc = info.doc_count;
        bc = info.block_count;
    }
    if (pool_) {
        cortrix::resource::NamespaceFacade facade(*pool_, name);
        if (facade.Acquire().ok()) {
            facade.store().doc_count(&dc);
            facade.store().block_count(&bc);
        }
    }
    (*out)["name"] = name;
    (*out)["created_at"] = created;
    (*out)["updated_at"] = updated;
    (*out)["doc_count"] = dc;
    (*out)["block_count"] = bc;
    return Status::Ok();
}

Status CortrixHttpServer::DeleteNamespaceUnified(const std::string& name) {
    // Catalog soft-delete also triggers F05 pool eviction (F12 §3.1.bis).
    if (ns_router_) return ns_router_->DeleteNamespace(name);
    return ns_mgr_.Delete(name);
}

void CortrixHttpServer::RegisterNamespaceRoutes() {
    // POST /api/v1/namespaces - Create namespace (ADMIN)
    if (config_.auth.enabled) {
        svr_.Post("/api/v1/namespaces", WithAuth(auth_, kPermAdmin,
            [this](const httplib::Request& req, httplib::Response& res, const RequestContext& rctx) {
                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch (...) {
                    WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"), rctx.request_id);
                    return;
                }

                if (!body.contains("name") || !body["name"].is_string()) {
                    WriteJsonError(res, Status::InvalidArgument("Missing 'name' field"), rctx.request_id);
                    return;
                }

                std::string name = body["name"].get<std::string>();

                nlohmann::json resp;
                if (ns_router_) {
                    // F13: route creation through the F12 catalog (single SoT). The
                    // router's Status carries the CX_ERR_NS_* token, so already-exists
                    // → 409 / quota → 403 fall out of WriteJsonError unchanged.
                    cortrix::catalog::NSMetadata meta;
                    meta.namespace_id = name;
                    meta.name         = name;
                    // P09 §4.6: tenant ownership from the auth context; the V1.0
                    // OSS single-tenant run (auth off / anonymous) falls back to
                    // the bootstrap 'default_tenant' (namespaces.tenant_id is
                    // NOT NULL + FK, seeded by the F12 schema bootstrap).
                    meta.tenant_id = rctx.auth.tenant_id.empty()
                                         ? "default_tenant" : rctx.auth.tenant_id;
                    Status s = ns_router_->CreateNamespace(meta);
                    if (!s.ok()) {
                        WriteJsonError(res, s, rctx.request_id);
                        return;
                    }
                    // Read the row back from the same SoT for created_at; a fresh NS
                    // has no documents yet (doc/block counts default to 0).
                    int64_t created_at = 0;
                    auto got = ns_router_->GetNamespace(name);
                    if (got.ok()) created_at = got.value().created_at;
                    resp["name"]        = name;
                    resp["created_at"]  = created_at;
                    resp["updated_at"]  = created_at;
                    resp["doc_count"]   = 0;
                    resp["block_count"] = 0;
                } else {
                    // No catalog router injected (standalone/tests): fall back to the
                    // metadata manager so the endpoint still functions.
                    Status s = ns_mgr_.Create(name);
                    if (!s.ok()) {
                        WriteJsonError(res, s, rctx.request_id);
                        return;
                    }
                    NamespaceInfo info;
                    ns_mgr_.Get(name, &info);
                    resp["name"]        = info.name;
                    resp["created_at"]  = info.created_at;
                    resp["updated_at"]  = info.updated_at;
                    resp["doc_count"]   = info.doc_count;
                    resp["block_count"] = info.block_count;
                }

                WriteJsonResponse(res, 201, resp, rctx.request_id);
            }
        ));
    } else {
        svr_.Post("/api/v1/namespaces", NoAuth(
            [this](const httplib::Request& req, httplib::Response& res, const RequestContext& rctx) {
                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch (...) {
                    WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"), rctx.request_id);
                    return;
                }

                if (!body.contains("name") || !body["name"].is_string()) {
                    WriteJsonError(res, Status::InvalidArgument("Missing 'name' field"), rctx.request_id);
                    return;
                }

                std::string name = body["name"].get<std::string>();

                nlohmann::json resp;
                if (ns_router_) {
                    // F13: route creation through the F12 catalog (single SoT). The
                    // router's Status carries the CX_ERR_NS_* token, so already-exists
                    // → 409 / quota → 403 fall out of WriteJsonError unchanged.
                    cortrix::catalog::NSMetadata meta;
                    meta.namespace_id = name;
                    meta.name         = name;
                    // P09 §4.6: tenant ownership from the auth context; the V1.0
                    // OSS single-tenant run (auth off / anonymous) falls back to
                    // the bootstrap 'default_tenant' (namespaces.tenant_id is
                    // NOT NULL + FK, seeded by the F12 schema bootstrap).
                    meta.tenant_id = rctx.auth.tenant_id.empty()
                                         ? "default_tenant" : rctx.auth.tenant_id;
                    Status s = ns_router_->CreateNamespace(meta);
                    if (!s.ok()) {
                        WriteJsonError(res, s, rctx.request_id);
                        return;
                    }
                    // Read the row back from the same SoT for created_at; a fresh NS
                    // has no documents yet (doc/block counts default to 0).
                    int64_t created_at = 0;
                    auto got = ns_router_->GetNamespace(name);
                    if (got.ok()) created_at = got.value().created_at;
                    resp["name"]        = name;
                    resp["created_at"]  = created_at;
                    resp["updated_at"]  = created_at;
                    resp["doc_count"]   = 0;
                    resp["block_count"] = 0;
                } else {
                    // No catalog router injected (standalone/tests): fall back to the
                    // metadata manager so the endpoint still functions.
                    Status s = ns_mgr_.Create(name);
                    if (!s.ok()) {
                        WriteJsonError(res, s, rctx.request_id);
                        return;
                    }
                    NamespaceInfo info;
                    ns_mgr_.Get(name, &info);
                    resp["name"]        = info.name;
                    resp["created_at"]  = info.created_at;
                    resp["updated_at"]  = info.updated_at;
                    resp["doc_count"]   = info.doc_count;
                    resp["block_count"] = info.block_count;
                }

                WriteJsonResponse(res, 201, resp, rctx.request_id);
            }
        ));
    }

    // GET /api/v1/namespaces - List namespaces (READ)
    if (config_.auth.enabled) {
        svr_.Get("/api/v1/namespaces", WithAuth(auth_, kPermRead,
            [this](const httplib::Request&, httplib::Response& res, const RequestContext& rctx) {
                nlohmann::json resp = BuildNamespaceListJson(rctx.auth.namespaces);
                WriteJsonResponse(res, 200, resp, rctx.request_id);
            }
        ));
    } else {
        svr_.Get("/api/v1/namespaces", NoAuth(
            [this](const httplib::Request&, httplib::Response& res, const RequestContext& rctx) {
                nlohmann::json resp = BuildNamespaceListJson({});
                WriteJsonResponse(res, 200, resp, rctx.request_id);
            }
        ));
    }

    // GET /api/v1/namespaces/:name - Get namespace (READ)
    if (config_.auth.enabled) {
        svr_.Get("/api/v1/namespaces/:name", WithAuth(auth_, kPermRead,
            [this](const httplib::Request& req, httplib::Response& res, const RequestContext& rctx) {
                std::string name = req.path_params.at("name");

                nlohmann::json resp;
                Status s = BuildNamespaceJson(name, &resp);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }

                WriteJsonResponse(res, 200, resp, rctx.request_id);
            }
        ));
    } else {
        svr_.Get("/api/v1/namespaces/:name", NoAuth(
            [this](const httplib::Request& req, httplib::Response& res, const RequestContext& rctx) {
                std::string name = req.path_params.at("name");

                nlohmann::json resp;
                Status s = BuildNamespaceJson(name, &resp);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }

                WriteJsonResponse(res, 200, resp, rctx.request_id);
            }
        ));
    }

    // DELETE /api/v1/namespaces/:name - Delete namespace (ADMIN)
    if (config_.auth.enabled) {
        svr_.Delete("/api/v1/namespaces/:name", WithAuth(auth_, kPermAdmin,
            [this](const httplib::Request& req, httplib::Response& res, const RequestContext& rctx) {
                std::string name = req.path_params.at("name");

                Status s = DeleteNamespaceUnified(name);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }

                res.set_header("X-Request-Id", rctx.request_id);
                res.status = 204;
            }
        ));
    } else {
        svr_.Delete("/api/v1/namespaces/:name", NoAuth(
            [this](const httplib::Request& req, httplib::Response& res, const RequestContext& rctx) {
                std::string name = req.path_params.at("name");

                Status s = DeleteNamespaceUnified(name);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }

                res.set_header("X-Request-Id", rctx.request_id);
                res.status = 204;
            }
        ));
    }
}

}  // namespace cortrix
