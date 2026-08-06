#include "cortrix/server/routes/import_routes.h"

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"
#include "cortrix/server/import_handler.h"

namespace cortrix {

namespace {

// Parse the request body into JSON; on malformed input returns a null json (the
// handler's ParseImportRequest maps that to CX_ERR_IMPORT_INVALID_SQL 400).
nlohmann::json ParseBodyOrNull(const httplib::Request& req) {
    try {
        return nlohmann::json::parse(req.body);
    } catch (...) {
        return nlohmann::json(nullptr);
    }
}

// Emit the handler's (body, http_status) pair as the HTTP response with the
// request-id correlation header.
void Emit(httplib::Response& res, const nlohmann::json& body, int http_status,
          const std::string& request_id) {
    if (!request_id.empty()) res.set_header("X-Request-Id", request_id);
    res.status = http_status;
    res.set_content(body.dump(), "application/json");
}

std::string PathParam(const httplib::Request& req, const char* key) {
    auto it = req.path_params.find(key);
    return it != req.path_params.end() ? it->second : "";
}

}  // namespace

void RegisterImportRoutes(httplib::Server& server, server::ImportHandler& handler,
                          ApiKeyAuth& auth) {
    // POST /api/v1/import/database — start import (async). Layer-2 admin via
    // WithAuth(kPermAdmin) (the /import/* prefix is not under AdminGuard).
    server.Post("/api/v1/import/database",
        WithAuth(auth, kPermAdmin,
            [&handler](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
                int http_status = 200;
                auto body = handler.HandleStartImport(ParseBodyOrNull(req), rctx.auth,
                                                      http_status);
                Emit(res, body, http_status, rctx.request_id);
            }));

    // GET /api/v1/import/tasks/:task_id/progress — progress query.
    server.Get("/api/v1/import/tasks/:task_id/progress",
        WithAuth(auth, kPermAdmin,
            [&handler](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
                int http_status = 200;
                auto body = handler.HandleGetProgress(PathParam(req, "task_id"),
                                                      http_status);
                Emit(res, body, http_status, rctx.request_id);
            }));

    // DELETE /api/v1/import/tasks/:task_id — cancel.
    server.Delete("/api/v1/import/tasks/:task_id",
        WithAuth(auth, kPermAdmin,
            [&handler](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
                int http_status = 200;
                auto body = handler.HandleCancel(PathParam(req, "task_id"), http_status);
                Emit(res, body, http_status, rctx.request_id);
            }));

    // POST /api/v1/admin/db-connections — register a connection. Under the
    // /api/v1/admin/* AdminGuard prefix (Layer 1) + handler.is_admin() (Layer 2).
    server.Post("/api/v1/admin/db-connections",
        WithAuth(auth, kPermAdmin,
            [&handler](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
                int http_status = 200;
                auto body = handler.HandleRegisterConnection(ParseBodyOrNull(req),
                                                             rctx.auth, http_status);
                Emit(res, body, http_status, rctx.request_id);
            }));

    // GET /api/v1/admin/db-connections — list connections (never the DSN).
    server.Get("/api/v1/admin/db-connections",
        WithAuth(auth, kPermAdmin,
            [&handler](const httplib::Request& /*req*/, httplib::Response& res,
                       const RequestContext& rctx) {
                int http_status = 200;
                auto body = handler.HandleListConnections(rctx.auth, http_status);
                Emit(res, body, http_status, rctx.request_id);
            }));

    // DELETE /api/v1/admin/db-connections/:ref_id — revoke a connection.
    server.Delete("/api/v1/admin/db-connections/:ref_id",
        WithAuth(auth, kPermAdmin,
            [&handler](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
                int http_status = 200;
                auto body = handler.HandleRevokeConnection(PathParam(req, "ref_id"),
                                                           ParseBodyOrNull(req),
                                                           rctx.auth, http_status);
                Emit(res, body, http_status, rctx.request_id);
            }));
}

}  // namespace cortrix
