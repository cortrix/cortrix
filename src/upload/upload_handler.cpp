#include <cstdint>
#include "cortrix/upload/upload_handler.h"
#include "cortrix/spc/spc_router.h"
#include "cortrix/logging/logging.h"

#include <openssl/evp.h>
#include <sstream>
#include <iomanip>

namespace cortrix {

UploadHandler::UploadHandler(const UploadConfig& config, SPCManager& spc_mgr)
    : config_(config), spc_mgr_(spc_mgr) {}

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

    if (found && existing_doc.content_hash == content_hash) {
        // Duplicate - skip
        result->doc_id = existing_doc.doc_id;
        result->content_hash = content_hash;
        result->status = "skipped";
        result->is_duplicate = true;
        return Status::Ok();
    }

    bool is_update = found;
    std::string old_doc_id = found ? existing_doc.doc_id : std::string();

    // Cancel old tasks if updating
    if (is_update) {
        spc_mgr_.CancelBySourcePath(req.filename);
    }

    // Create Document
    CortrixDoc doc;
    doc.source_type = "http_upload";
    doc.source_path = req.filename;
    doc.content_hash = content_hash;
    doc.file_size = static_cast<int64_t>(req.file_size);
    doc.mime_type = mime_type;
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

    CORTRIX_LOG_INFO("upload", "Upload success: doc_id={}, filename={}, status={}",
                    doc.doc_id, req.filename, result->status);
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

}  // namespace cortrix
