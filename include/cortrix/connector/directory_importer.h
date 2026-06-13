#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include "cortrix/common/status.h"
#include "cortrix/config/config.h"
#include "cortrix/connector/file_watcher.h"
#include "cortrix/connector/file_filter.h"
#include "cortrix/spc/spc_manager.h"

namespace cortrix {

// D3.5 wire⑤c: the directory importer acquires its namespace through the F05
// resource pool (per-operation NamespaceFacade), not the MVP manager.
namespace resource { class INamespacePool; }

struct ImportStats {
    int64_t total_files = 0;
    int64_t imported = 0;           // new + updated
    int64_t skipped_unchanged = 0;  // content_hash unchanged
    int64_t skipped_filtered = 0;   // filtered by rules
    int64_t skipped_error = 0;      // processing error
    int64_t deleted = 0;            // cascade-deleted files
    int64_t updated = 0;           // content_hash changed
    double elapsed_s = 0.0;

    std::string ToString() const;
};

class DirectoryImporter {
public:
    /// @param config: watch_dir configuration
    /// @param pool: F05 namespace resource pool (per-op NamespaceFacade acquire)
    /// @param spc_mgr: SPC pipeline manager
    DirectoryImporter(const WatchDirConfig& config,
                      cortrix::resource::INamespacePool& pool,
                      SPCManager& spc_mgr);
    ~DirectoryImporter();

    /// Start initial scan (file processing only — NO OS watcher).
    /// 1. Resolve + validate data_dir, build the file filter.
    /// 2. Recursive scan of data_dir.
    /// 3. For each file: filter -> hash -> incremental check -> doc_create -> SPC enqueue.
    ///
    /// F21: OS-level file watching is owned by DirWatcherRegistry (1 watcher per
    /// directory, fan-out to all subscribed namespaces). DirectoryImporter no
    /// longer creates a FileWatcher; it is a pure per-namespace file processor.
    /// Real-time events arrive via HandleFileEvents() called by the registry.
    ///
    /// @return Ok / NotFound(data_dir not found) / Internal
    Status Start();

    /// Mark importer inactive (idempotent). The OS watcher lifecycle is owned by
    /// DirWatcherRegistry; this only flips the active flag.
    void Stop();

    /// Get import statistics
    ImportStats GetStats() const;

    /// F21: Handle a batch of file events fanned out by DirWatcherRegistry.
    /// Each importer independently merges events, then processes (hash check +
    /// SPC enqueue) or cascade-deletes. Made public (was private in MVP) so the
    /// registry can dispatch one OS event batch to every subscribed namespace.
    void HandleFileEvents(const std::vector<FileEvent>& events);

    /// Whether this importer is active (Start() succeeded and not Stop()'d).
    /// F21: no longer reflects OS watcher state (that is owned by the registry);
    /// reflects whether the importer is processing events for its namespace.
    bool IsWatching() const;

    /// Get current configuration
    const WatchDirConfig& GetConfig() const { return config_; }

    /// Delete ALL documents indexed from this watch directory (cascade: blocks + HNSW + blob).
    /// Call before removing the watcher when the user wants to purge its content from the KB.
    /// @return number of documents deleted, or -1 on error
    int DeleteAllDocs();

private:
    /// Recursive directory scan
    Status InitialScan();

    /// Process a single file (import or skip)
    Status ProcessFile(const std::string& file_path);

    /// Handle file deletion (cascade delete doc + blocks + hnsw + blob)
    Status HandleFileDeletion(const std::string& file_path);

    /// After initial scan, remove DB docs whose files no longer exist on disk.
    /// Handles deletions that occurred while the server was offline (missed by FSEvents).
    void CleanupStaleDocs();

    WatchDirConfig config_;
    cortrix::resource::INamespacePool& pool_;
    SPCManager& spc_mgr_;

    // F21: FileWatcher ownership moved to DirWatcherRegistry (1 per directory,
    // fan-out to N namespaces). DirectoryImporter keeps only the per-namespace
    // file filter + stats and is driven by HandleFileEvents().
    std::unique_ptr<FileFilter> filter_;
    ImportStats stats_;
    mutable std::mutex stats_mu_;
    std::atomic<bool> running_{false};
};

}  // namespace cortrix
