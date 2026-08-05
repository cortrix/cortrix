#pragma once
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_context.h"
#include "cortrix/common/result.h"
#include "cortrix/import/connection_manager.h"
#include "cortrix/import/import_manager.h"
#include "cortrix/import/import_types.h"

namespace cortrix::server {

/// HTTP handler core for the 6 database-import endpoints. This is the request-
/// parse + response-build logic the D3.5 httplib routes call; binding it into the
/// real `httplib::Server` (+ AdminGuard middleware §6.1 two-layer protection) is D3.5 wiring, so
/// here it is a plain, fully-unit-testable class over the import/connection managers.
///
/// Each method returns the JSON body to serialize + sets `out_http_status` to the
/// §5.4 / §6.2 HTTP code. Errors are the Agent-friendly body (§5.3); the parse layer
/// maps a malformed request to CX_ERR_IMPORT_INVALID_SQL (400).
class ImportHandler {
public:
    ImportHandler(std::shared_ptr<cortrix::import::ImportManager> import_mgr,
                  std::shared_ptr<cortrix::import::IConnectionManager> conn_mgr);

    /// Parse a request body into an ImportRequest (oneOf{table, sql} +
    /// text_strategy). Returns CX_ERR_IMPORT_INVALID_SQL on a missing required field,
    /// an unknown text_strategy ("template" is rejected — v1.0.2 ruling 13), or a
    /// malformed shape.
    static cortrix::Result<cortrix::import::ImportRequest> ParseImportRequest(
        const nlohmann::json& body);

    // POST /api/v1/import/database — start import (async). 200 / 400 / 403.
    nlohmann::json HandleStartImport(const nlohmann::json& body,
                                     const AuthContext& auth_ctx,
                                     int& out_http_status);

    // GET /api/v1/import/tasks/{task_id}/progress — 200 / 404.
    nlohmann::json HandleGetProgress(const std::string& task_id, int& out_http_status);

    // DELETE /api/v1/import/tasks/{task_id} — cancel. 200 / 404.
    nlohmann::json HandleCancel(const std::string& task_id, int& out_http_status);

    // POST /api/v1/admin/db-connections — register (D1). 200 / 400 / 403.
    nlohmann::json HandleRegisterConnection(const nlohmann::json& body,
                                            const AuthContext& auth_ctx,
                                            int& out_http_status);

    // GET /api/v1/admin/db-connections — list. 200 / 403.
    nlohmann::json HandleListConnections(const AuthContext& auth_ctx, int& out_http_status);

    // DELETE /api/v1/admin/db-connections/{ref_id} — revoke. 200 / 403 / 404.
    nlohmann::json HandleRevokeConnection(const std::string& ref_id,
                                          const nlohmann::json& body,
                                          const AuthContext& auth_ctx,
                                          int& out_http_status);

private:
    std::shared_ptr<cortrix::import::ImportManager> import_mgr_;
    std::shared_ptr<cortrix::import::IConnectionManager> conn_mgr_;
};

}  // namespace cortrix::server
