#pragma once

namespace httplib { class Server; }

namespace cortrix {
class ApiKeyAuth;
namespace server { class ImportHandler; }

/// Register the 6 DB-import endpoints on
/// the raw httplib server, delegating to the (borrowed) ImportHandler:
///   POST   /api/v1/import/database               -- start import (async)
///   GET    /api/v1/import/tasks/{task_id}/progress -- progress
///   DELETE /api/v1/import/tasks/{task_id}        -- cancel
///   POST   /api/v1/admin/db-connections          -- register a connection
///   GET    /api/v1/admin/db-connections          -- list connections
///   DELETE /api/v1/admin/db-connections/{ref_id} -- revoke a connection
///
/// Two-layer admin protection:
///   Layer 1 — AdminGuard IP filter. /admin/db-connections* match the
///     /api/v1/admin/* prefix directly; the /import/* paths are a business prefix,
///     so they rely on Layer 2 only (per design — Phase 1.5 unifies them).
///   Layer 2 — admin-role RBAC. All 6 are WithAuth(kPermAdmin); the handler also
///     re-checks AuthContext.is_admin() for the connection ops (CX_ERR_AUTH_ADMIN_*).
///
/// `handler` must outlive `server`.
void RegisterImportRoutes(httplib::Server& server, server::ImportHandler& handler,
                          ApiKeyAuth& auth);

}  // namespace cortrix
