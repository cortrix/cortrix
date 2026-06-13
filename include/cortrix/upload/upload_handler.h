#pragma once
#include <string>
#include <memory>
#include <cstdint>
#include "cortrix/common/status.h"
#include "cortrix/common/data_types.h"
#include "cortrix/config/config.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/store/cortrix_blob_store.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/spc_task.h"
#include "cortrix/spc/spc_router.h"

namespace cortrix {

struct UploadRequest {
    std::string namespace_name;       // target namespace (URL path param)
    std::string filename;             // original filename (multipart Content-Disposition)
    std::string mime_type;            // MIME type
    const char* file_data = nullptr;  // file content pointer (cpp-httplib multipart buffer)
    size_t file_size = 0;             // file size (bytes)
    std::string metadata_json;        // user-defined metadata (optional)
    std::string title;                // document title (optional, default=filename)
};

struct UploadResult {
    std::string doc_id;               // ULID (D-I6)
    std::string content_hash;         // SHA-256 hex
    std::string status;               // "pending" | "skipped" | "updating"
    bool is_duplicate = false;        // true = content_hash unchanged, skip processing
};

class UploadHandler {
public:
    /// @param config: upload configuration
    /// @param spc_mgr: SPC pipeline manager (task enqueue)
    UploadHandler(const UploadConfig& config, SPCManager& spc_mgr);

    /// Handle single file upload.
    ///
    /// Takes the narrow per-request windows the handler actually uses (the caller
    /// — a route handler — obtains them from a per-request F05 NamespaceFacade and
    /// passes facade.store()/facade.blob()). The target namespace for blob keying
    /// is carried on req.namespace_name.
    ///
    /// @param req: upload request
    /// @param store: metadata store window (doc CRUD)
    /// @param blob: blob store window (raw file bytes)
    /// @param result: [out] upload result
    /// @return Ok / InvalidArgument(415/413) / NotFound(404) / Internal(500)
    Status HandleUpload(const UploadRequest& req,
                        CortrixStore& store,
                        CortrixBlobStore& blob,
                        UploadResult* result);

    /// Query document status. Takes the metadata store window (facade.store()).
    Status GetDocumentStatus(CortrixStore& store,
                             const std::string& doc_id,
                             CortrixDoc* doc);

private:
    static std::string ComputeContentHash(const void* data, size_t len);
    Status ValidateFileSize(size_t file_size) const;
    static Status ValidateMimeType(const std::string& mime_type);
    static std::string ResolveMimeType(const std::string& provided_mime,
                                        const std::string& filename);
    SPCPriority DeterminePriority(size_t file_size) const;

    std::shared_ptr<SPCTask> CreateSpcTask(
        const std::string& doc_id,
        const std::string& namespace_name,
        const std::string& filename,
        const std::string& content_hash,
        const std::string& mime_type,
        SPCPriority priority,
        bool is_update,
        const std::string& old_doc_id,
        const std::string& metadata_json = "");

    bool FindExistingDocument(CortrixStore& store,
                              const std::string& filename,
                              CortrixDoc* existing_doc);

    UploadConfig config_;
    SPCManager& spc_mgr_;
};

}  // namespace cortrix
