#pragma once

#include <mutex>
#include <memory>
#include <vector>
#include <string>

namespace httplib { class Server; }

namespace cortrix {

class DirectoryImporter;
class ApiKeyAuth;
class NamespaceManager;
class SPCManager;

// D3.5 wire⑤c: connector routes acquire per-operation NamespaceFacades over the
// F05 resource pool, and route namespace creation through the F12 catalog router.
namespace resource { class INamespacePool; }
namespace catalog  { class INSRouter; }

struct WatcherEntry {
    std::string id;                              ///< Deterministic 8-hex-char ID (hash of data_dir)
    std::unique_ptr<DirectoryImporter> importer;
    // non-copyable due to unique_ptr
};

/// Shared mutable state for connector routes (thread-safe via mutex)
struct ConnectorState {
    std::mutex mu;
    std::vector<WatcherEntry> watchers;          ///< All active directory watchers
    std::string data_dir;                        ///< Data root for watchers.json persistence
};

class Status;

/// Add (or replace, when (dir, ns) is already watched) a single directory watcher
/// and return its entry. Shared by the /connector/* routes and the design-surface
/// /watch aliases (flat_document_routes) so both mount the SAME watcher mechanism.
/// Caller MUST hold `state.mu`. `out_entry` (if non-null) receives the new entry.
Status ConnectorAddWatcher(ConnectorState& state,
                           cortrix::catalog::INSRouter& ins_router,
                           cortrix::resource::INamespacePool& pool,
                           SPCManager& spc_mgr,
                           const std::string& data_dir,
                           const std::string& ns_name,
                           WatcherEntry** out_entry,
                           bool autostart = true);

/// Persist the current watcher list to {state.data_dir}/watchers.json.
/// Caller MUST hold `state.mu`. Shared with the /watch aliases.
void ConnectorSaveWatchers(const ConnectorState& state);

/// Register connector HTTP routes
///
/// Multi-watcher endpoints:
///   GET    /api/v1/connector/watchers           -- list all watchers with status + stats
///   POST   /api/v1/connector/watchers           -- add a new watcher (keeps existing ones)
///   DELETE /api/v1/connector/watchers/:id       -- remove a specific watcher
///   POST   /api/v1/connector/watchers/:id/scan  -- trigger scan for a specific watcher
///
/// Backward-compat single-watcher endpoints:
///   GET    /api/v1/connector/status             -- status of first watcher
///   POST   /api/v1/connector/watch              -- replace ALL watchers with one
///   GET    /api/v1/connector/stats              -- aggregate stats across all watchers
///   POST   /api/v1/connector/scan               -- scan all watchers
///
/// Directory picker:
///   GET    /api/v1/browse?path=<dir>            -- list subdirectories
///
void RegisterConnectorRoutes(httplib::Server& server,
                              ConnectorState& state,
                              cortrix::resource::INamespacePool& pool,
                              cortrix::catalog::INSRouter& ins_router,
                              NamespaceManager& meta_mgr,
                              SPCManager& spc_mgr,
                              ApiKeyAuth& auth);

}  // namespace cortrix
