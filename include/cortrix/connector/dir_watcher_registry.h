#pragma once
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/connector/directory_importer.h"
#include "cortrix/connector/file_watcher.h"

namespace cortrix {

namespace resource { class INamespacePool; }  // D3.5 wire⑤c: F05 pool dependency
class NamespaceManager;
class SPCManager;

/// F21 Watcher Fan-out (TD-WATCHER-001).
///
/// MVP created one OS-level FileWatcher per (directory, namespace) pair: when a
/// single directory was subscribed by N namespaces it spawned N OS watchers and
/// performed N redundant initial scans. DirWatcherRegistry refactors this into a
/// fan-out model: exactly one FileWatcher per directory whose events are fanned
/// out to every subscribed namespace's DirectoryImporter.
///
/// Phase 1 only `recursive` is configurable per watcher; the remaining options
/// (file_patterns / auto_delete / allowed_extensions) are Phase 2 per-namespace
/// work (TD-WATCHER-OPTIONS). ignore_patterns is fixed internally to
/// {".*", "*~", "*.tmp", "*.swp", "#*#"}.
struct WatchOptions {
    bool recursive = true;
};

/// Public view of a watched directory (returned by ListWatches / GetWatch).
struct WatchInfo {
    std::string directory;                       ///< canonical absolute path
    std::vector<std::string> target_namespaces;  ///< all namespaces subscribed to this directory
    bool watching = false;                       ///< whether the OS watcher is running
    int subscriber_count = 0;                    ///< == target_namespaces.size()
    bool recursive = true;                        ///< whether subdirectories are watched
    std::string id;                              ///< watcher id (hash of canonical_dir)
};

class DirWatcherRegistry {
public:
    DirWatcherRegistry(cortrix::resource::INamespacePool& pool,
                       NamespaceManager& meta_mgr,
                       SPCManager& spc_mgr);
    ~DirWatcherRegistry();

    DirWatcherRegistry(const DirWatcherRegistry&) = delete;
    DirWatcherRegistry& operator=(const DirWatcherRegistry&) = delete;

    // --- Subscription management ---

    /// Subscribe one or more namespaces to a directory. Creates the watcher if
    /// the directory has none, otherwise appends the namespaces to the existing
    /// watcher (idempotent per namespace).
    /// @param directory         directory path (canonicalized internally)
    /// @param target_namespaces namespaces to subscribe (>= 1)
    /// @param recursive          watch subdirectories (only honored when a NEW
    ///                           watcher is created; ignored for an existing one)
    /// @param autostart          start the OS watcher + initial scan immediately
    ///                           (false is used by LoadState to defer to StartAll)
    /// @return Ok / NotFound(directory) / InvalidArgument(empty namespaces) /
    ///         Internal(watcher init failed)
    Status Subscribe(const std::string& directory,
                     const std::vector<std::string>& target_namespaces,
                     bool recursive = true,
                     bool autostart = true);

    /// Unsubscribe a single namespace from a directory. When the last namespace
    /// is removed the watcher is stopped and the directory entry destroyed.
    /// @return Ok / NotFound(directory not watched, or namespace not subscribed)
    Status Unsubscribe(const std::string& directory,
                       const std::string& namespace_id);

    /// Remove the whole watcher for a directory (unsubscribe every namespace).
    /// @return Ok / NotFound
    Status RemoveWatch(const std::string& directory);

    // --- Queries ---

    std::vector<WatchInfo> ListWatches() const;

    /// Look a watcher up by its id (hash of the canonical path).
    std::optional<WatchInfo> GetWatch(const std::string& id) const;

    // --- Control ---

    /// Trigger a full rescan of a directory; results fan out to every subscriber.
    /// @return Ok / NotFound(directory not watched)
    Status TriggerScan(const std::string& directory);

    /// Start every registered-but-not-yet-running watcher (service startup).
    void StartAll();

    /// Stop every running watcher.
    void StopAll();

    // --- Persistence ---

    /// Persist all watchers to {data_dir}/watchers.json (schema version 2).
    Status SaveState(const std::string& data_dir) const;

    /// Restore watchers from {data_dir}/watchers.json. Accepts the v1 (per
    /// (dir, ns)) layout and merges it into v2 (per dir, [ns...]). Restored
    /// watchers are created with autostart=false; call StartAll() to launch.
    /// A missing or corrupt file is a non-fatal warning (returns Ok).
    Status LoadState(const std::string& data_dir);

    /// Watcher id derivation: SHA-256(canonical_dir) truncated to 8 hex chars.
    /// Used as the REST path parameter (DELETE .../watchers/:id). Exposed for
    /// callers (e.g. API routes) that need to resolve an id without a WatchInfo.
    static std::string MakeId(const std::string& canonical_dir);

private:
    /// One entry per watched directory: a single OS watcher fanning out to a
    /// per-namespace importer list.
    struct WatcherSlot {
        std::string canonical_dir;
        std::vector<std::string> target_namespaces;        ///< subscriber list
        bool recursive = true;
        std::unique_ptr<FileWatcher> watcher;               ///< 1 OS watcher per dir
        std::vector<std::unique_ptr<DirectoryImporter>> importers;  ///< 1 per namespace
        std::string id;                                     ///< hash of canonical_dir
    };

    /// Find an existing slot by canonical path (mu_ must be held). nullptr if none.
    WatcherSlot* FindSlot(const std::string& canonical_dir);
    const WatcherSlot* FindSlot(const std::string& canonical_dir) const;

    /// Build a WatchInfo snapshot from a slot (mu_ must be held).
    static WatchInfo MakeInfo(const WatcherSlot& slot);

    /// Lazily start the OS watcher for a slot (mu_ must be held). No-op if it is
    /// already running. Wires the watcher callback to FanOutEvents(slot).
    Status EnsureWatcherStarted(WatcherSlot& slot);

    /// Fan a batch of OS events out to every importer in the slot. Applies the
    /// fixed ignore_patterns filter once, then dispatches the surviving events to
    /// each importer (events are shared read-only; no per-namespace copy needed).
    void FanOutEvents(WatcherSlot& slot, const std::vector<FileEvent>& events);

    cortrix::resource::INamespacePool& pool_;
    NamespaceManager& meta_mgr_;
    SPCManager& spc_mgr_;

    mutable std::mutex mu_;
    std::vector<std::unique_ptr<WatcherSlot>> slots_;  ///< one slot per directory

    // Test-only friend for deterministic fan-out coverage (mirrors the
    // MemorySearcherPrivateTest pattern) — lets the unit test drive FanOutEvents
    // without depending on real OS-watcher event timing.
    friend class DirWatcherRegistryPrivateTest;
};

}  // namespace cortrix
