#include <cstdint>
#include "cortrix/upload/upload_handler.h"
#include "cortrix/spc/spc_router.h"
#include "cortrix/logging/logging.h"
#include "cortrix/observability/operation_log_emitter.h"

#include <openssl/evp.h>
#include <sstream>
#include <iomanip>
#include <utility>
#include <vector>

namespace cortrix {

UploadHandler::UploadHandler(const UploadConfig& config, SPCManager& spc_mgr,
                             std::shared_ptr<observability::IOperationLogger> op_logger)
    : config_(config), spc_mgr_(spc_mgr), op_logger_(std::move(op_logger)) {}

void UploadHandler::EmitUploadLog(const std::string& namespace_name,
                                  const std::string& doc_id,
                                  const std::string& filename,
                                  const std::string& status) {
    if (!op_logger_) return;  // observability strictly additive (C4)
    // §9.1 SpcPipeline → resource_type "document"; resource_id = doc_id. Summary
    // carries the filename + outcome (skipped / pending / updating) and is truncated
    // to ≤100 by the emitter. user_id / trace_id / session_id come from the
    // thread-local ObservabilityContext.
    auto entry = observability::MakeEngineEntry(
        observability::EmitSite::kSpcPipeline, "upload", namespace_name,
        /*resource_id=*/doc_id,
        "upload " + status + ": " + filename);
    op_logger_->Log(entry);
}

std::string UploadHandler::ComputeContentHash(const void* data, size_t len) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }
    EVP_MD_CTX_free(ctx);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return ss.str();
}

Status UploadHandler::ValidateFileSize(size_t file_size) const {
    if (static_cast<int64_t>(file_size) > config_.max_file_size) {
        return Status::InvalidArgument(
            "file too large: " + std::to_string(file_size) +
            " > " + std::to_string(config_.max_file_size));
    }
    return Status::Ok();
}

Status UploadHandler::ValidateMimeType(const std::string& mime_type) {
    if (!SPCRouter::IsSupported(mime_type)) {
        return Status::InvalidArgument("unsupported media type: " + mime_type);
    }
    return Status::Ok();
}

std::string UploadHandler::ResolveMimeType(const std::string& provided_mime,
                                            const std::string& filename) {
    if (!provided_mime.empty() && provided_mime != "application/octet-stream") {
        return provided_mime;
    }
    std::string inferred = SPCRouter::InferMimeType(filename);
    if (!inferred.empty() && inferred != "application/octet-stream") {
        return inferred;
    }
    return provided_mime.empty() ? "application/octet-stream" : provided_mime;
}

SPCPriority UploadHandler::DeterminePriority(size_t file_size) const {
    if (static_cast<int64_t>(file_size) >= config_.large_file_threshold) {
        return SPCPriority::kP2;
    }
    return SPCPriority::kP0;
}

bool UploadHandler::FindExistingDocument(CortrixStore& store,
                                          const std::string& filename,
                                          CortrixDoc* existing_doc) {
    int rc = store.doc_find_by_source("http_upload", filename, *existing_doc);
    if (rc == 0) {
        return true;
    }
    if (rc != -2) {
        CORTRIX_LOG_WARN("upload", "doc_find_by_source error: rc={}", rc);
    }
    return false;
}

std::shared_ptr<SPCTask> UploadHandler::CreateSpcTask(
    const std::string& doc_id,
    const std::string& namespace_name,
    const std::string& filename,
    const std::string& content_hash,
    const std::string& mime_type,
    SPCPriority priority,
    bool is_update,
    const std::string& old_doc_id,
    const std::string& metadata_json) {

    auto task = std::make_shared<SPCTask>();
    task->doc_id = doc_id;
    task->namespace_name = namespace_name;
    task->source_path = filename;
    task->source_type = "http_upload";
    task->content_hash = content_hash;
    task->mime_type = mime_type;
    task->processing_level = SPCRouter::InferProcessingLevel(mime_type);
    task->priority = priority;
    task->is_update = is_update;
    task->old_doc_id = old_doc_id;
    task->metadata_json = metadata_json;
    task->stage = SPCStage::kQueued;
    return task;
}

Status UploadHandler::HandleUpload(const UploadRequest& req,
                                    CortrixStore& store,
                                    CortrixBlobStore& blob,
                                    UploadResult* result) {
    // Validate file size
    Status s = ValidateFileSize(req.file_size);
    if (!s.ok()) return s;

    // Resolve and validate MIME type
    std::string mime_type = ResolveMimeType(req.mime_type, req.filename);
    s = ValidateMimeType(mime_type);
    if (!s.ok()) return s;

    // Compute content hash
    std::string content_hash = ComputeContentHash(req.file_data, req.file_size);
    if (content_hash.empty()) {
        return Status::Internal("SHA-256 computation failed");
    }

    // Incremental detection
    CortrixDoc existing_doc;
    bool found = FindExistingDocument(store, req.filename, &existing_doc);

    // Unchanged content is skipped only while that version is, or will be, processed:
    // it is ready, or a task for it is queued or running. A version with the same
    // content and no such future (a failed submit, a crash, a cancel, a failed run, or a
    // version left processing after its task ended) is replaced by a new version,
    // exactly as changed content is. The row is read again when no task is active, so
    // a version whose task published it in between is still skipped.
    const bool same_content = found && existing_doc.content_hash == content_hash;
    bool will_be_processed =
        same_content && (existing_doc.status == DocStatus::kReady ||
                         spc_mgr_.HasActiveTask(existing_doc.doc_id));
    if (same_content && !will_be_processed) {
        CortrixDoc again;
        if (store.doc_get(existing_doc.doc_id, again) == 0 &&
            again.status == DocStatus::kReady) {
            will_be_processed = true;
        }
    }
    if (will_be_processed) {
        // Duplicate - skip
        result->doc_id = existing_doc.doc_id;
        result->content_hash = content_hash;
        result->status = "skipped";
        result->is_duplicate = true;
        // [F18a §9.1] The upload operation completed successfully (idempotent no-op);
        // record it so the Agent sees the call landed against the existing doc.
        EmitUploadLog(req.namespace_name, existing_doc.doc_id, req.filename, "skipped");
        return Status::Ok();
    }

    bool is_update = found;
    std::string old_doc_id = found ? existing_doc.doc_id : std::string();

    // Cancel old tasks if updating
    if (is_update) {
        spc_mgr_.CancelQueuedVersions(req.namespace_name, "http_upload", req.filename);
    }

    // Create Document
    CortrixDoc doc;
    doc.source_type = "http_upload";
    doc.source_path = req.filename;
    doc.content_hash = content_hash;
    doc.file_size = static_cast<int64_t>(req.file_size);
    doc.mime_type = mime_type;
    // The row carries the task's processing level (as the async path's row does),
    // so the enrichment audit and the backfill read the level the ingest used.
    doc.processing_level = SPCRouter::InferProcessingLevel(mime_type);
    doc.status = DocStatus::kPending;
    doc.title = req.title.empty() ? req.filename : req.title;
    doc.metadata_json = req.metadata_json;

    int rc = store.doc_create(doc);
    if (rc != 0) {
        CORTRIX_LOG_ERROR("upload", "doc_create failed: rc={}", rc);
        return Status::Internal("failed to create document");
    }

    // Store blob
    rc = blob.store(req.namespace_name, doc.doc_id, req.file_data, req.file_size);
    if (rc != 0) {
        CORTRIX_LOG_ERROR("upload", "blob store failed: rc={}, doc_id={}", rc, doc.doc_id);
        store.doc_delete(doc.doc_id);
        return Status::Internal("failed to store file");
    }

    // Create and submit SPC task
    SPCPriority priority = DeterminePriority(req.file_size);
    auto task = CreateSpcTask(doc.doc_id, req.namespace_name, req.filename,
                               content_hash, mime_type, priority, is_update, old_doc_id,
                               req.metadata_json);

    s = spc_mgr_.Submit(task);
    if (!s.ok()) {
        CORTRIX_LOG_WARN("upload", "SPC submit failed: {}", s.message());
        result->doc_id = doc.doc_id;
        result->content_hash = content_hash;
        result->status = is_update ? "updating" : "pending";
        result->is_duplicate = false;
        result->superseded_doc_id = is_update ? old_doc_id : std::string();
        // [gap②] Pass the disk-full rejection through unchanged — masking it as
        // "queue full" would mislead the caller (F24 §6 wants CX_ERR_DISK_FULL
        // surfaced). The doc + blob are already saved at this point either way.
        if (s.message().find("CX_ERR_DISK_FULL") != std::string::npos) {
            return s;
        }
        return Status::Unavailable("processing queue full, document saved");
    }

    result->doc_id = doc.doc_id;
    result->content_hash = content_hash;
    result->status = is_update ? "updating" : "pending";
    result->is_duplicate = false;
    result->superseded_doc_id = is_update ? old_doc_id : std::string();

    CORTRIX_LOG_INFO("upload", "Upload success: doc_id={}, filename={}, status={}",
                    doc.doc_id, req.filename, result->status);
    // [F18a §9.1] Record the accepted upload (doc created + SPC task queued). The
    // SPC-submit-failed path above returns a non-Ok Status and is deliberately NOT
    // logged (operation_log records successful operations only, §4.1).
    EmitUploadLog(req.namespace_name, doc.doc_id, req.filename, result->status);
    return Status::Ok();
}

Status UploadHandler::GetDocumentStatus(CortrixStore& store,
                                         const std::string& doc_id,
                                         CortrixDoc* doc) {
    int rc = store.doc_get(doc_id, *doc);
    if (rc == 0) {
        return Status::Ok();
    }
    if (rc == -2) {
        return Status::NotFound("document not found: " + doc_id);
    }
    CORTRIX_LOG_ERROR("upload", "doc_get failed: rc={}, doc_id={}", rc, doc_id);
    return Status::Internal("failed to query document status");
}

int UploadHandler::RequeuePendingUploads(CortrixStore& store,
                                         const std::string& namespace_name) {
    std::vector<CortrixDoc> pending;
    if (store.doc_list_by_status(DocStatus::kPending, pending) != 0) {
        CORTRIX_LOG_WARN("upload", "requeue: listing pending documents failed, ns={}",
                         namespace_name);
        return -1;
    }

    int requeued = 0;
    for (const CortrixDoc& doc : pending) {
        if (doc.source_type != "http_upload") continue;

        // Only the newest live version of the file. An older one was superseded by a
        // later upload, and the newest version retires it when it is ready.
        CortrixDoc newest;
        if (!FindExistingDocument(store, doc.source_path, &newest) ||
            newest.doc_id != doc.doc_id) {
            continue;
        }

        // A task already queued or running for it is left alone. The row is read
        // again when no task is active, so a task that finished in between is seen.
        if (spc_mgr_.HasActiveTask(doc.doc_id)) continue;
        CortrixDoc again;
        if (store.doc_get(doc.doc_id, again) != 0 || again.status != DocStatus::kPending) {
            continue;
        }

        auto task = CreateSpcTask(doc.doc_id, namespace_name, doc.source_path,
                                  doc.content_hash, doc.mime_type,
                                  DeterminePriority(static_cast<size_t>(doc.file_size)),
                                  /*is_update=*/false, /*old_doc_id=*/"", doc.metadata_json);
        Status s = spc_mgr_.Submit(task);
        if (!s.ok()) {
            CORTRIX_LOG_WARN("upload", "requeue: SPC submit failed, doc_id={}: {}",
                             doc.doc_id, s.message());
            continue;
        }
        ++requeued;
    }

    if (requeued > 0) {
        CORTRIX_LOG_INFO("upload", "requeued {} pending upload(s) in ns={}", requeued,
                         namespace_name);
    }
    return requeued;
}

}  // namespace cortrix
