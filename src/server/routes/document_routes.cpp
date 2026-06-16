#include <cstdint>
#include "cortrix/server/routes/document_routes.h"
#include "cortrix/upload/upload_handler.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"
#include "cortrix/logging/logging.h"
#include "cortrix/deploy/disk_monitor.h"     // [D3.5 gap②] disk pressure gate
#include "cortrix/agent_friendly/error.h"    // CX_ERR_DISK_FULL body serialization

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <string>

namespace cortrix {

namespace {

std::string DocStatusToString(DocStatus status) {
    switch (status) {
        case DocStatus::kPending:    return "pending";
        case DocStatus::kProcessing: return "processing";
        case DocStatus::kReady:      return "ready";
        case DocStatus::kError:      return "error";
        case DocStatus::kStale:      return "stale";
        case DocStatus::kDeleting:   return "deleting";
        case DocStatus::kDeleted:    return "deleted";
        default:                     return "unknown";
    }
}

int MapStatusToHttp(const Status& status) {
    if (status.code() == StatusCode::kInvalidArgument) {
        if (status.message().find("file too large") != std::string::npos) {
            return 413;
        }
        if (status.message().find("unsupported media type") != std::string::npos) {
            return 415;
        }
        return 400;
    }
    if (status.code() == StatusCode::kUnavailable) {
        return 503;
    }
    return status.http_status();
}

}  // namespace

void RegisterDocumentRoutes(httplib::Server& server,
                             UploadHandler& handler,
                             resource::INamespacePool& pool,
                             ApiKeyAuth& auth,
                             const deploy::DiskMonitor* disk_monitor) {

    // POST /api/v1/namespaces/:name/documents — File upload
    server.Post("/api/v1/namespaces/:name/documents",
        WithAuth(auth, kPermWrite,
            [&handler, &pool, disk_monitor](const httplib::Request& req, httplib::Response& res,
                              const RequestContext& rctx) {
                // [D3.5 wire · gap②] Disk pressure gate (F24 §6, F24-4 decision A):
                // at CRIT reject the upload BEFORE any byte is written (doc_create /
                // blob.store below would only deepen the pressure). 507 + the full
                // CX_ERR_DISK_FULL Agent-friendly body.
                if (disk_monitor && disk_monitor->ShouldRejectWrites()) {
                    WriteJsonResponse(res, 507,
                        agent_friendly::ToJson(disk_monitor->MakeDiskFullError()),
                        rctx.request_id);
                    return;
                }

                // Extract namespace
                auto ns_it = req.path_params.find("name");
                if (ns_it == req.path_params.end()) {
                    WriteJsonError(res, Status::InvalidArgument("missing namespace"), rctx.request_id);
                    return;
                }
                std::string ns_name = ns_it->second;

                // Acquire namespace (per-request façade over the F05 pool;
                // released by the destructor when this handler returns).
                resource::NamespaceFacade facade(pool, ns_name);
                Status acq = facade.Acquire();
                if (!acq.ok()) {
                    WriteJsonError(res, Status::NotFound(
                        "namespace not found: " + ns_name + ": " + acq.message()),
                        rctx.request_id);
                    return;
                }

                // Extract file from multipart
                if (!req.has_file("file")) {
                    WriteJsonError(res, Status::InvalidArgument("missing file field"), rctx.request_id);
                    return;
                }

                const auto& file = req.get_file_value("file");
                if (file.filename.empty()) {
                    WriteJsonError(res, Status::InvalidArgument("missing filename"), rctx.request_id);
                    return;
                }

                if (req.files.count("file") > 1) {
                    CORTRIX_LOG_WARN("upload", "multiple files received, using first");
                }

                // Build UploadRequest
                UploadRequest upload_req;
                upload_req.namespace_name = ns_name;
                upload_req.filename = file.filename;
                upload_req.mime_type = file.content_type;
                upload_req.file_data = file.content.data();
                upload_req.file_size = file.content.size();

                // Extract optional fields
                if (req.has_file("metadata")) {
                    upload_req.metadata_json = req.get_file_value("metadata").content;
                }
                if (req.has_file("title")) {
                    upload_req.title = req.get_file_value("title").content;
                }

                // Handle upload — pass the narrow per-request windows from the façade.
                UploadResult result;
                Status s = handler.HandleUpload(upload_req, facade.store(), facade.blob(), &result);

                if (!s.ok()) {
                    int http_code = MapStatusToHttp(s);
                    // Use standard error envelope per API_GATEWAY_DESIGN.md 2.5.1
                    // WriteJsonError includes timestamp per spec
                    WriteJsonError(res, s, rctx.request_id);
                    // Override HTTP status for special codes (413, 415)
                    if (http_code != s.http_status()) {
                        res.status = http_code;
                    }
                    return;
                }

                // Build success response
                nlohmann::json body;
                body["doc_id"] = result.doc_id;
                body["content_hash"] = result.content_hash;
                body["status"] = result.status;
                body["source_path"] = upload_req.filename;

                if (result.is_duplicate) {
                    body["message"] = "Document unchanged, skipping reprocessing";
                    res.status = 200;
                } else if (result.status == "updating") {
                    body["message"] = "Document updated, reprocessing queued";
                    res.status = 201;
                } else {
                    body["message"] = "Document uploaded successfully, processing queued";
                    res.status = 201;
                }
                res.set_content(body.dump(), "application/json");
            }
        )
    );

    // GET /api/v1/namespaces/:name/documents/:id/status — Query document status
    server.Get("/api/v1/namespaces/:name/documents/:id/status",
        WithAuth(auth, kPermRead,
            [&handler, &pool](const httplib::Request& req, httplib::Response& res,
                              const RequestContext& rctx) {
                // Extract namespace
                auto ns_it = req.path_params.find("name");
                if (ns_it == req.path_params.end()) {
                    WriteJsonError(res, Status::InvalidArgument("missing namespace"), rctx.request_id);
                    return;
                }
                std::string ns_name = ns_it->second;

                // Acquire namespace (per-request façade over the F05 pool;
                // released by the destructor when this handler returns).
                resource::NamespaceFacade facade(pool, ns_name);
                Status acq = facade.Acquire();
                if (!acq.ok()) {
                    WriteJsonError(res, Status::NotFound(
                        "namespace not found: " + ns_name + ": " + acq.message()),
                        rctx.request_id);
                    return;
                }

                // Extract and parse doc_id
                auto id_it = req.path_params.find("id");
                if (id_it == req.path_params.end()) {
                    WriteJsonError(res, Status::InvalidArgument("missing doc_id"), rctx.request_id);
                    return;
                }

                // doc_id is a ULID string (D-I6); use the path param directly.
                const std::string& doc_id = id_it->second;
                if (doc_id.empty()) {
                    WriteJsonError(res, Status::InvalidArgument("invalid doc_id"), rctx.request_id);
                    return;
                }

                // Query document status
                CortrixDoc doc;
                Status s = handler.GetDocumentStatus(facade.store(), doc_id, &doc);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }
                // [OPEN-2] A soft-deleted doc (Stage 1) is invisible to clients: 404
                // until it is either restored or hard-deleted. The row still exists
                // (restorable within the retention window) but is not externally live.
                if (doc.status == DocStatus::kDeleted) {
                    WriteJsonError(res, Status::NotFound("document not found: " + doc_id),
                                   rctx.request_id);
                    return;
                }

                // Build response
                nlohmann::json body;
                body["doc_id"] = doc.doc_id;
                body["source_type"] = doc.source_type;
                body["source_path"] = doc.source_path;
                body["status"] = DocStatusToString(doc.status);
                body["error_message"] = doc.error_message;
                body["block_count"] = doc.block_count;
                body["content_hash"] = doc.content_hash;
                body["mime_type"] = doc.mime_type;
                body["file_size"] = doc.file_size;
                body["created_at"] = doc.created_at;
                body["updated_at"] = doc.updated_at;
                body["processed_at"] = doc.processed_at;

                WriteJsonResponse(res, 200, body, rctx.request_id);
            }
        )
    );

    // GET /api/v1/namespaces/:name/documents — List all documents
    server.Get("/api/v1/namespaces/:name/documents",
        WithAuth(auth, kPermRead,
            [&pool](const httplib::Request& req, httplib::Response& res,
                    const RequestContext& rctx) {
                auto ns_it = req.path_params.find("name");
                if (ns_it == req.path_params.end()) {
                    WriteJsonError(res, Status::InvalidArgument("missing namespace"), rctx.request_id);
                    return;
                }
                std::string ns_name = ns_it->second;

                // Acquire namespace (per-request façade over the F05 pool;
                // released by the destructor when this handler returns).
                resource::NamespaceFacade facade(pool, ns_name);
                Status acq = facade.Acquire();
                if (!acq.ok()) {
                    WriteJsonError(res, Status::NotFound(
                        "namespace not found: " + ns_name + ": " + acq.message()),
                        rctx.request_id);
                    return;
                }

                // Collect docs from all statuses
                std::vector<CortrixDoc> all_docs;
                static const DocStatus kStatuses[] = {
                    DocStatus::kPending, DocStatus::kProcessing,
                    DocStatus::kReady, DocStatus::kError, DocStatus::kStale
                };
                for (auto status : kStatuses) {
                    std::vector<CortrixDoc> docs;
                    facade.store().doc_list_by_status(status, docs);
                    all_docs.insert(all_docs.end(), docs.begin(), docs.end());
                }

                // Sort by doc_id descending (newest first)
                std::sort(all_docs.begin(), all_docs.end(),
                    [](const CortrixDoc& a, const CortrixDoc& b) {
                        return a.doc_id > b.doc_id;
                    });

                // Optional pagination
                int limit = 50;
                int offset = 0;
                if (req.has_param("limit")) {
                    try { limit = std::min(std::stoi(req.get_param_value("limit")), 200); } catch (...) {}
                }
                if (req.has_param("offset")) {
                    try { offset = std::max(std::stoi(req.get_param_value("offset")), 0); } catch (...) {}
                }

                nlohmann::json docs_array = nlohmann::json::array();
                int total = static_cast<int>(all_docs.size());
                int end = std::min(offset + limit, total);
                for (int i = offset; i < end; ++i) {
                    const auto& doc = all_docs[i];
                    // Get real block count from blocks table
                    int64_t real_block_count = doc.block_count;
                    if (real_block_count == 0) {
                        facade.store().block_count_by_doc(doc.doc_id, &real_block_count);
                    }
                    nlohmann::json d;
                    d["doc_id"] = doc.doc_id;
                    d["source_type"] = doc.source_type;
                    d["source_path"] = doc.source_path;
                    d["status"] = DocStatusToString(doc.status);
                    d["block_count"] = real_block_count;
                    d["content_hash"] = doc.content_hash;
                    d["mime_type"] = doc.mime_type;
                    d["file_size"] = doc.file_size;
                    d["created_at"] = doc.created_at;
                    d["updated_at"] = doc.updated_at;
                    docs_array.push_back(d);
                }

                nlohmann::json body;
                body["documents"] = docs_array;
                body["total_count"] = total;
                WriteJsonResponse(res, 200, body, rctx.request_id);
            }
        )
    );

    // DELETE /api/v1/namespaces/:name/documents/:id — Delete document + blocks
    server.Delete("/api/v1/namespaces/:name/documents/:id",
        WithAuth(auth, kPermWrite,
            [&handler, &pool](const httplib::Request& req, httplib::Response& res,
                              const RequestContext& rctx) {
                auto ns_it = req.path_params.find("name");
                if (ns_it == req.path_params.end()) {
                    WriteJsonError(res, Status::InvalidArgument("missing namespace"), rctx.request_id);
                    return;
                }
                std::string ns_name = ns_it->second;

                // Acquire namespace (per-request façade over the F05 pool;
                // released by the destructor when this handler returns).
                resource::NamespaceFacade facade(pool, ns_name);
                Status acq = facade.Acquire();
                if (!acq.ok()) {
                    WriteJsonError(res, Status::NotFound(
                        "namespace not found: " + ns_name + ": " + acq.message()),
                        rctx.request_id);
                    return;
                }

                auto id_it = req.path_params.find("id");
                if (id_it == req.path_params.end()) {
                    WriteJsonError(res, Status::InvalidArgument("missing doc_id"), rctx.request_id);
                    return;
                }

                // doc_id is a ULID string (D-I6); use the path param directly.
                const std::string& doc_id = id_it->second;
                if (doc_id.empty()) {
                    WriteJsonError(res, Status::InvalidArgument("invalid doc_id"), rctx.request_id);
                    return;
                }

                // Verify doc exists (and is not already soft-deleted → 404).
                CortrixDoc doc;
                Status s = handler.GetDocumentStatus(facade.store(), doc_id, &doc);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }
                if (doc.status == DocStatus::kDeleted) {
                    WriteJsonError(res, Status::NotFound("document not found: " + doc_id),
                                   rctx.request_id);
                    return;
                }

                // [OPEN-2] Stage 1 soft delete: mark the doc 'deleted' + stamp
                // deleted_at. Blocks / vectors / blob stay in place (LIST + retrieval
                // filter the doc out; restorable within the retention window). The
                // GC sweeper hard-deletes blocks + blob + the row at Stage 2 once the
                // window elapses (gc.soft_delete_retention_days; 0 = immediate).
                int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                int rc = facade.store().doc_soft_delete(doc_id, now_ms);
                if (rc != 0) {
                    WriteJsonError(res, Status::Internal("failed to delete document"),
                                   rctx.request_id);
                    return;
                }
                // 204 No Content (team-lead): DELETE is idempotent + body-less.
                res.status = 204;
            }
        )
    );
}

}  // namespace cortrix
