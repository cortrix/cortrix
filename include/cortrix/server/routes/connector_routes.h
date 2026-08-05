#pragma once

#include <memory>
#include <string>

namespace httplib { class Server; }

namespace cortrix {

class DirWatcherRegistry;
class ApiKeyAuth;
class NamespaceManager;
class SPCManager;

// D3.5 wire⑤c: connector routes acquire per-operation NamespaceFacades over the
// resource pool, and route namespace creation+admission through the catalog and
// catalog router (the DirWatcherRegistry holds these two; the MVP NamespaceManager
// is no longer on the watcher path).
namespace resource { class INamespacePool; }
namespace catalog  { class INSRouter; }

/// Shared state for the connector routes.
///
/// Watcher fan-out: the MVP `vector<WatcherEntry>` (one OS FileWatcher per
/// (dir, namespace) pair) is replaced by a single DirWatcherRegistry that keeps
/// exactly one OS watcher per directory and fans its events out to every
/// subscribed namespace. The registry is internally synchronized (its own mu_),
/// so this struct no longer carries a mutex or a watcher vector.
///
/// `registry` is created lazily by RegisterConnectorRoutes (the only place that
/// has the pool + NamespaceManager + SPCManager it needs). bootstrap.cpp
/// constructs the struct, sets `data_dir`, then calls RegisterConnectorRoutes /
/// RegisterWatchAliasRoutes which share the SAME ConnectorState, so both the
/// /connector/* routes and the /watch aliases drive one registry.
struct ConnectorState {
    std::string data_dir;                          ///< root for watchers.json persistence
    std::unique_ptr<DirWatcherRegistry> registry;  ///< fan-out registry (lazily built)
};

/// Lazily create `state.registry` if it is null, wiring it to the pool, the
/// catalog router and the SPC pipeline, then restore persisted watchers from
/// {state.data_dir}/watchers.json (v1 layouts auto-migrate to v2). Idempotent:
/// a second call with an already-built registry is a no-op. Shared by the
/// /connector/* routes and the /watch aliases so both mount the SAME registry.
/// After this returns, callers should invoke `state.registry->StartAll()` once
/// the server is ready to begin OS watching + the initial scan (bootstrap owns
/// that call, mirroring the MVP startup loop).
void ConnectorEnsureRegistry(ConnectorState& state,
                             cortrix::resource::INamespacePool& pool,
                             cortrix::catalog::INSRouter& ins_router,
                             SPCManager& spc_mgr);

/// Register connector HTTP routes (fan-out model).
///
/// Multi-watcher endpoints:
///   GET    /api/v1/connector/watchers                     -- list watchers (per-NS stats)
///   POST   /api/v1/connector/watchers                     -- subscribe target_namespaces[] to a dir
///   DELETE /api/v1/connector/watchers/:id                 -- remove a watcher (all NS) + purge docs
///   DELETE /api/v1/connector/watchers/:id/namespaces/:ns  -- unsubscribe a single NS
///   POST   /api/v1/connector/watchers/:id/scan            -- trigger a fan-out rescan
///
/// Backward-compat single-watcher endpoints:
///   GET    /api/v1/connector/status   -- status of the first watcher
///   POST   /api/v1/connector/watch    -- replace ALL watchers with one (single NS)
///   GET    /api/v1/connector/stats    -- aggregate stats across all watchers
///   POST   /api/v1/connector/scan     -- rescan all watchers
///
/// Directory picker:
///   GET    /api/v1/browse?path=<dir>  -- list subdirectories
///
/// `pool`, `ins_router` and `spc_mgr` must outlive `server`; they are captured
/// into `state.registry`. (The MVP NamespaceManager param was dropped — namespace
/// creation/admission now goes through `ins_router`.)
void RegisterConnectorRoutes(httplib::Server& server,
                              ConnectorState& state,
                              cortrix::resource::INamespacePool& pool,
                              cortrix::catalog::INSRouter& ins_router,
                              SPCManager& spc_mgr,
                              ApiKeyAuth& auth);

}  // namespace cortrix
