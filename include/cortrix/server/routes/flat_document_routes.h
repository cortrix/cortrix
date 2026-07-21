#pragma once

namespace httplib { class Server; }

namespace cortrix {

namespace resource { class INamespacePool; }
namespace catalog  { class INSRouter; }
namespace server { class BatchSubmitService; }
namespace async { class DocumentTaskHandler; }

class UploadHandler;
class ApiKeyAuth;
class SPCManager;
class NamespaceManager;
struct ConnectorState;

/// Register the flat (design-surface) /documents family + /watch aliases that the
/// openapi spec (api/paths/documents.yaml + watch.yaml), the P03 Python SDK
/// (resources/documents.py + watchers.py) and cortrix-mcp (tools/core.py) all wire
/// to. Live storage is per-namespace (the nested /namespaces/:name/documents +
/// /connector/* routes); these flat routes mount the design surface ON TOP of the
/// existing live handlers without removing the nested/compat routes.
///
/// Endpoints (all under /api/v1):
///   POST   /documents                         -- async upload (JSON body) -> 202 DocumentTask
///   GET    /documents?namespace=&limit=&offset= -- list (per-NS facade)
///   GET    /documents/{id}?namespace=          -- get one (per-NS storage -> ?namespace= required)
///   DELETE /documents/{id}?namespace=          -- soft delete (GC lifecycle) -> 204
///   GET    /documents/tasks/{task_id}/progress -- F42 task progress
///   DELETE /documents/tasks/{task_id}          -- F42 task cancel
///
/// `batch_service` (borrowed) fans a single upload out through the frozen F42
/// scheduler as a batch-of-1 (reusing its inline-content materialization), and
/// `task_handler` (borrowed) owns the F42 progress/cancel bodies. Both must
/// outlive the server. `disk_monitor` may be null (no disk gating).
void RegisterFlatDocumentRoutes(httplib::Server& server,
                                resource::INamespacePool& pool,
                                UploadHandler& handler,
                                server::BatchSubmitService& batch_service,
                                async::DocumentTaskHandler& task_handler,
                                ApiKeyAuth& auth);

/// Register the /watch alias family that maps the spec/SDK/MCP fan-out shape
/// ({path, target_namespaces[], recursive}) onto the F21 DirWatcherRegistry. With
/// F21 a directory fanning out to N namespaces is ONE registry watcher carrying
/// all N target_namespaces (the MVP keyed by (dir, ns) and emitted N watchers).
/// The nested /connector/* routes are kept and share the SAME registry.
///
/// Endpoints (all under /api/v1):
///   POST   /watch              -- subscribe target_namespaces[] to a dir -> 201 Watcher
///   GET    /watch              -- list watchers -> {watchers:[...]}
///   DELETE /watch/{id}         -- remove a watch (+ purge docs) -> 204
///
/// NOTE: GET /watch/{id}/events from the spec has NO live event-store implementation
/// in the connector today, so it is intentionally NOT mounted here (recorded in the
/// Wave S2 spec-reconcile). `state`, `pool`, `ins_router` and `spc_mgr` are the same
/// objects the /connector/* routes own; this function calls ConnectorEnsureRegistry
/// so it is independent of /connector route registration order (idempotent — only
/// one registry per ConnectorState). All must outlive the server.
void RegisterWatchAliasRoutes(httplib::Server& server,
                              ConnectorState& state,
                              resource::INamespacePool& pool,
                              catalog::INSRouter& ins_router,
                              SPCManager& spc_mgr,
                              ApiKeyAuth& auth);

}  // namespace cortrix
