#include <cstdint>
#include "cortrix/connector/directory_importer.h"
#include "cortrix/connector/file_utils.h"
#include "cortrix/logging/logging.h"
#include "cortrix/resource/namespace_facade.h"  // integration wire⑤c: per-op NamespaceFacade
#include <filesystem>
#include <chrono>
#include <unordered_map>

namespace cortrix {

static const char* kModule = "DirImporter";

// --- ImportStats ---

std::string ImportStats::ToString() const {
    return fmt::format(
        "total={} imported={} updated={} skipped_unchanged={} "
        "skipped_filtered={} skipped_error={} deleted={} elapsed={:.2f}s",
        total_files, imported, updated, skipped_unchanged,
        skipped_filtered, skipped_error, deleted, elapsed_s);
}

// --- DirectoryImporter ---

DirectoryImporter::DirectoryImporter(const WatchDirConfig& config,
                                     cortrix::resource::INamespacePool& pool,
                                     SPCManager& spc_mgr)
    : config_(config), pool_(pool), spc_mgr_(spc_mgr) {
    // Build the file filter at construction. The filter depends only on
    // config_ (fixed for this importer's lifetime), so it must be ready even
    // before Start() — DirWatcherRegistry may fan events into HandleFileEvents()
    // before an initial scan (e.g. when subscribed with autostart=false).
    FilterConfig fc;
    fc.ignore_patterns = config_.ignore_patterns;
    fc.allowed_extensions = config_.allowed_extensions;
    filter_ = std::make_unique<FileFilter>(fc);
}

DirectoryImporter::~DirectoryImporter() {
    Stop();
}

Status DirectoryImporter::Start() {
    if (config_.data_dir.empty()) {
        CORTRIX_LOG_INFO(kModule, "no data_dir configured, directory import disabled");
        return Status::Ok();
    }

    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists(config_.data_dir, ec)) {
        return Status::NotFound("data_dir not found: " + config_.data_dir);
    }
    if (!fs::is_directory(config_.data_dir, ec)) {
        return Status::InvalidArgument("data_dir is not a directory: " + config_.data_dir);
    }

    // Resolve to absolute path
    auto abs_path = fs::canonical(config_.data_dir, ec);
    if (ec) {
        return Status::Internal("failed to resolve data_dir: " + ec.message());
    }
    config_.data_dir = abs_path.string();

    // filter_ is built in the constructor (config-only dependency); no need to
    // recreate it here.

    // Initial scan
    auto scan_status = InitialScan();
    if (!scan_status.ok()) {
        return scan_status;
    }

    // OS watcher creation removed — DirWatcherRegistry owns a single
    // FileWatcher per directory and fans events out to every subscribed
    // namespace's importer via HandleFileEvents(). The importer is "active"
    // once its initial scan has completed and is ready to process events.
    running_ = true;
    CORTRIX_LOG_INFO(kModule, "importer ready for ns={} dir={}",
                    config_.namespace_name, config_.data_dir);

    return Status::Ok();
}

void DirectoryImporter::Stop() {
    // No OS watcher to stop here (owned by DirWatcherRegistry). Idempotent.
    running_ = false;
}

ImportStats DirectoryImporter::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mu_);
    return stats_;
}

bool DirectoryImporter::IsWatching() const {
    return running_.load();
}

Status DirectoryImporter::InitialScan() {
    namespace fs = std::filesystem;

    auto start_time = std::chrono::steady_clock::now();
    CORTRIX_LOG_INFO(kModule, "starting initial scan: {}", config_.data_dir);

    std::vector<std::string> files_to_process;
    int64_t total_files = 0;
    int64_t skipped_filtered = 0;

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             config_.data_dir,
             fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); ) {

        if (ec) {
            CORTRIX_LOG_WARN(kModule, "directory scan error: {}", ec.message());
            ec.clear();
            ++it;
            continue;
        }

        const auto& entry = *it;

        if (entry.is_directory(ec)) {
            std::string dir_name = entry.path().filename().string();
            if (!filter_->ShouldTraverse(dir_name)) {
                it.disable_recursion_pending();
            }
            ++it;
            continue;
        }

        if (!entry.is_regular_file(ec)) {
            ++it;
            continue;
        }

        total_files++;
        std::string path = entry.path().string();

        if (!filter_->ShouldImport(path)) {
            skipped_filtered++;
            ++it;
            continue;
        }

        files_to_process.push_back(path);
        ++it;
    }

    {
        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.total_files = total_files;
        stats_.skipped_filtered = skipped_filtered;
    }

    CORTRIX_LOG_INFO(kModule, "scan found {} files, {} to process, {} filtered",
                    total_files, files_to_process.size(), skipped_filtered);

    // Process files in batches
    int64_t processed = 0;
    int consecutive_errors = 0;
    for (const auto& file_path : files_to_process) {
        auto status = ProcessFile(file_path);
        processed++;

        if (!status.ok()) {
            consecutive_errors++;
            if (config_.error_threshold > 0 &&
                consecutive_errors >= config_.error_threshold) {
                CORTRIX_LOG_ERROR(kModule,
                    "error_threshold ({}) reached after {} consecutive errors, aborting scan",
                    config_.error_threshold, consecutive_errors);
                break;
            }
        } else {
            consecutive_errors = 0;
        }

        if (config_.scan_batch_size > 0 &&
            processed % config_.scan_batch_size == 0) {
            std::lock_guard<std::mutex> lock(stats_mu_);
            CORTRIX_LOG_INFO(kModule, "scan progress: {}/{} processed, {} imported, {} skipped",
                            processed, files_to_process.size(),
                            stats_.imported, stats_.skipped_unchanged);
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.elapsed_s = std::chrono::duration<double>(end_time - start_time).count();
    }

    // After processing existing files, clean up docs whose files were deleted
    // while the server was offline (FSEvents would have missed those deletions).
    CleanupStaleDocs();

    auto final_stats = GetStats();
    CORTRIX_LOG_INFO(kModule, "initial scan complete: {}", final_stats.ToString());

    return Status::Ok();
}

void DirectoryImporter::CleanupStaleDocs() {
    cortrix::resource::NamespaceFacade facade(pool_, config_.namespace_name);
    cortrix::Status acq = facade.Acquire();
    if (!acq.ok()) return;

    // Normalize prefix: no trailing slash
    std::string prefix = config_.data_dir;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

    // Collect all watch_dir docs under this data_dir from the DB
    std::vector<CortrixDoc> candidates;
    for (auto status : {DocStatus::kReady, DocStatus::kPending,
                        DocStatus::kProcessing, DocStatus::kError, DocStatus::kStale}) {
        std::vector<CortrixDoc> batch;
        facade.store().doc_list_by_status(status, batch);
        for (auto& d : batch) {
            if (d.source_type != "watch_dir") continue;
            if (d.source_path == prefix ||
                d.source_path.substr(0, prefix.size() + 1) == prefix + "/") {
                candidates.push_back(std::move(d));
            }
        }
    }

    namespace fs = std::filesystem;
    int64_t removed = 0;
    for (const auto& doc : candidates) {
        std::error_code ec;
        if (!fs::exists(doc.source_path, ec)) {
            HandleFileDeletion(doc.source_path);
            removed++;
        }
    }

    if (removed > 0) {
        CORTRIX_LOG_INFO(kModule, "stale doc cleanup: {} file(s) removed from DB (deleted while offline): {}",
                        removed, config_.data_dir);
    }
}

Status DirectoryImporter::ProcessFile(const std::string& file_path) {
    // edge case #5: resolve symlinks to canonical path to avoid duplicates
    std::string resolved = ResolvePath(file_path);
    if (resolved.empty()) {
        CORTRIX_LOG_WARN(kModule, "failed to resolve path: {}", file_path);
        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.skipped_error++;
        return Status::Internal("path resolution failed");
    }
    const std::string& effective_path = resolved;

    // Acquire the namespace bundle for the lifetime of this operation (the
    // façade's store() view is valid only while the façade lives).
    cortrix::resource::NamespaceFacade facade(pool_, config_.namespace_name);
    cortrix::Status acq = facade.Acquire();
    if (!acq.ok()) {
        CORTRIX_LOG_ERROR(kModule, "namespace acquire failed: {}: {}",
                          config_.namespace_name, acq.message());
        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.skipped_error++;
        return Status::NotFound("namespace not found");
    }

    // Compute file hash
    std::string content_hash;
    int rc = ComputeFileHash(effective_path, content_hash);
    if (rc != 0) {
        CORTRIX_LOG_WARN(kModule, "failed to compute hash for: {}", effective_path);
        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.skipped_error++;
        return Status::Internal("hash computation failed");
    }

    // Check if document already exists (using resolved path for dedup)
    CortrixDoc existing_doc;
    rc = facade.store().doc_find_by_source("watch_dir", effective_path, existing_doc);

    if (rc == -2) {
        // New file - create document and enqueue SPC
        int64_t file_size = GetFileSize(effective_path);
        std::string mime_type = DetectMimeType(effective_path);

        CortrixDoc doc;
        doc.source_type = "watch_dir";
        doc.source_path = effective_path;
        doc.source_ref = config_.data_dir;
        doc.content_hash = content_hash;
        doc.file_size = file_size;
        doc.mime_type = mime_type;
        doc.status = DocStatus::kPending;
        doc.title = GetBasename(effective_path);
        doc.processing_level = 3;

        rc = facade.store().doc_create(doc);
        if (rc != 0) {
            CORTRIX_LOG_WARN(kModule, "doc_create failed for: {}", effective_path);
            std::lock_guard<std::mutex> lock(stats_mu_);
            stats_.skipped_error++;
            return Status::Internal("doc_create failed");
        }

        // Enqueue SPC task
        auto task = std::make_shared<SPCTask>();
        task->doc_id = doc.doc_id;
        task->namespace_name = config_.namespace_name;
        task->source_path = effective_path;
        task->source_type = "watch_dir";
        task->content_hash = content_hash;
        task->mime_type = mime_type;
        task->priority = SPCPriority::kP2;  // batch import = low priority
        task->is_update = false;

        auto submit_status = spc_mgr_.Submit(task);
        if (!submit_status.ok()) {
            CORTRIX_LOG_WARN(kModule, "SPC submit failed for doc_id={}: {}",
                            doc.doc_id, submit_status.message());
        }

        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.imported++;
        return Status::Ok();

    } else if (rc == 0) {
        // Existing document - check for content change
        if (existing_doc.content_hash == content_hash) {
            // Hash unchanged - but if status is pending/error, the doc was never
            // successfully processed (e.g., server crashed mid-import). Re-enqueue.
            if (existing_doc.status == DocStatus::kPending ||
                existing_doc.status == DocStatus::kError) {
                CORTRIX_LOG_INFO(kModule, "re-enqueue unprocessed doc: doc_id={}, status={}, path={}",
                                existing_doc.doc_id,
                                existing_doc.status == DocStatus::kPending ? "pending" : "error",
                                effective_path);
                auto task = std::make_shared<SPCTask>();
                task->doc_id = existing_doc.doc_id;
                task->namespace_name = config_.namespace_name;
                task->source_path = effective_path;
                task->source_type = "watch_dir";
                task->content_hash = content_hash;
                task->mime_type = existing_doc.mime_type;
                task->priority = SPCPriority::kP2;
                task->is_update = false;
                spc_mgr_.Submit(task);

                std::lock_guard<std::mutex> lock(stats_mu_);
                stats_.imported++;
                return Status::Ok();
            }

            CORTRIX_LOG_DEBUG(kModule, "file unchanged: {}", effective_path);
            std::lock_guard<std::mutex> lock(stats_mu_);
            stats_.skipped_unchanged++;
            return Status::Ok();
        }

        // Content changed - update
        spc_mgr_.CancelBySourcePath(effective_path);

        facade.store().doc_update_status(existing_doc.doc_id, DocStatus::kStale);
        facade.store().doc_update_status(existing_doc.doc_id, DocStatus::kPending);

        auto task = std::make_shared<SPCTask>();
        task->doc_id = existing_doc.doc_id;
        task->namespace_name = config_.namespace_name;
        task->source_path = effective_path;
        task->source_type = "watch_dir";
        task->content_hash = content_hash;
        task->mime_type = DetectMimeType(effective_path);  // Re-detect: format may have changed
        task->priority = SPCPriority::kP1;  // watch change = medium priority
        task->is_update = true;
        task->old_doc_id = existing_doc.doc_id;

        auto submit_status = spc_mgr_.Submit(task);
        if (!submit_status.ok()) {
            CORTRIX_LOG_WARN(kModule, "SPC submit failed for update doc_id={}: {}",
                            existing_doc.doc_id, submit_status.message());
        }

        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.updated++;
        return Status::Ok();
    }

    // Other error from doc_find_by_source
    CORTRIX_LOG_WARN(kModule, "doc_find_by_source error for: {}", effective_path);
    std::lock_guard<std::mutex> lock(stats_mu_);
    stats_.skipped_error++;
    return Status::Internal("doc_find_by_source failed");
}

void DirectoryImporter::HandleFileEvents(const std::vector<FileEvent>& events) {
    // Event merging: deduplicate by path
    std::unordered_map<std::string, FileEventType> merged;

    for (const auto& ev : events) {
        if (ev.is_directory) continue;
        if (!filter_->ShouldImport(ev.path)) continue;

        auto it = merged.find(ev.path);
        if (it == merged.end()) {
            merged[ev.path] = ev.type;
        } else {
            // Merge logic
            auto prev = it->second;
            auto curr = ev.type;

            if (prev == FileEventType::kCreated && curr == FileEventType::kDeleted) {
                merged.erase(it);  // temp file, ignore
            } else if (prev == FileEventType::kCreated && curr == FileEventType::kModified) {
                // keep as Created
            } else if (prev == FileEventType::kModified && curr == FileEventType::kModified) {
                // keep latest Modified
            } else if (prev == FileEventType::kModified && curr == FileEventType::kDeleted) {
                it->second = FileEventType::kDeleted;
            } else {
                it->second = curr;
            }
        }
    }

    // Process merged events
    for (const auto& [path, type] : merged) {
        if (type == FileEventType::kCreated || type == FileEventType::kModified) {
            ProcessFile(path);
        } else if (type == FileEventType::kDeleted) {
            HandleFileDeletion(path);
        }
    }
}

Status DirectoryImporter::HandleFileDeletion(const std::string& file_path) {
    cortrix::resource::NamespaceFacade facade(pool_, config_.namespace_name);
    cortrix::Status acq = facade.Acquire();
    if (!acq.ok()) {
        return Status::NotFound("namespace not found");
    }

    CortrixDoc doc;
    int rc = facade.store().doc_find_by_source("watch_dir", file_path, doc);
    if (rc == -2) {
        CORTRIX_LOG_DEBUG(kModule, "no document for deleted file: {}", file_path);
        return Status::Ok();
    }
    if (rc != 0) {
        return Status::Internal("doc_find_by_source failed");
    }

    // Cancel any in-flight SPC tasks
    spc_mgr_.CancelBySourcePath(file_path);

    // Per design: set Document status to 'deleting' before cascade
    int urc = facade.store().doc_update_status(doc.doc_id, DocStatus::kDeleting);
    if (urc != 0) {
        CORTRIX_LOG_WARN(kModule, "failed to set kDeleting for doc_id={}", doc.doc_id);
        // Continue with best-effort cascade despite status update failure
    }

    // Cascade delete: blocks (vector index entries) + document. The pool / P-HNSW key
    // is block_id; MarkDelete is idempotent (a missing id returns Ok), so we
    // delete every block unconditionally — no hnsw_node_id guard needed.
    std::vector<CortrixBlock> blocks;
    facade.store().block_get_by_doc(doc.doc_id, blocks);

    int hnsw_errors = 0;
    for (const auto& block : blocks) {
        cortrix::Status vrc = facade.vec_index().MarkDelete(block.block_id);
        if (!vrc.ok()) {
            CORTRIX_LOG_WARN(kModule, "vector MarkDelete failed for block_id={}: {}",
                             block.block_id, vrc.message());
            hnsw_errors++;
        }
    }

    // Delete document (cascades to blocks via SQLite)
    int drc = facade.store().doc_delete(doc.doc_id);
    if (drc != 0) {
        CORTRIX_LOG_ERROR(kModule, "doc_delete failed for doc_id={}, cascade incomplete", doc.doc_id);
        return Status::Internal("doc_delete failed during cascade");
    }

    // Defensive blob cleanup (watch_dir typically has no blob)
    facade.blob().del(config_.namespace_name, doc.doc_id);

    CORTRIX_LOG_INFO(kModule, "file deleted, cascade cleanup: doc_id={}, path={}",
                    doc.doc_id, file_path);

    {
        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.deleted++;
    }

    return Status::Ok();
}

int DirectoryImporter::DeleteAllDocs() {
    cortrix::resource::NamespaceFacade facade(pool_, config_.namespace_name);
    cortrix::Status acq = facade.Acquire();
    if (!acq.ok()) {
        CORTRIX_LOG_WARN(kModule, "DeleteAllDocs: namespace acquire failed: {}: {}",
                         config_.namespace_name, acq.message());
        return -1;
    }

    // Normalize prefix: no trailing slash
    std::string prefix = config_.data_dir;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

    // Collect all docs across all statuses, filter by watch_dir + path prefix
    std::vector<CortrixDoc> candidates;
    for (auto status : {DocStatus::kReady, DocStatus::kPending,
                        DocStatus::kProcessing, DocStatus::kError, DocStatus::kStale}) {
        std::vector<CortrixDoc> batch;
        facade.store().doc_list_by_status(status, batch);
        for (auto& d : batch) {
            if (d.source_type != "watch_dir") continue;
            if (d.source_path == prefix ||
                d.source_path.substr(0, prefix.size() + 1) == prefix + "/") {
                candidates.push_back(std::move(d));
            }
        }
    }

    int deleted = 0;
    for (const auto& doc : candidates) {
        Status s = HandleFileDeletion(doc.source_path);
        if (s.ok()) {
            deleted++;
        } else {
            CORTRIX_LOG_WARN(kModule, "DeleteAllDocs: failed path={}: {}", doc.source_path, s.message());
        }
    }

    CORTRIX_LOG_INFO(kModule, "DeleteAllDocs: {} doc(s) removed from {}", deleted, config_.data_dir);
    return deleted;
}

}  // namespace cortrix
