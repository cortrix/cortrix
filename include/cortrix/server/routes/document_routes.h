#pragma once

namespace httplib { class Server; }

namespace cortrix {

namespace resource { class INamespacePool; }
namespace deploy { class DiskMonitor; }

class UploadHandler;
class ApiKeyAuth;

/// Register document HTTP routes
///
/// Endpoints:
///   POST   /api/v1/namespaces/:ns/documents            -- file upload
///   GET    /api/v1/namespaces/:ns/documents             -- list all documents
///   GET    /api/v1/namespaces/:ns/documents/:id/status  -- query processing status
///   DELETE /api/v1/namespaces/:ns/documents/:id         -- delete document + blocks
///
/// [integration wire · gap②] `disk_monitor` (optional): when set, the upload endpoint
/// consults DiskMonitor::ShouldRejectWrites() BEFORE any byte is written
/// (doc_create / blob.store) and rejects with 507 + the CX_ERR_DISK_FULL
/// Agent-friendly body (reject all new writes at
/// CRIT). nullptr = no disk gating (tests / minimal deployments).
void RegisterDocumentRoutes(httplib::Server& server,
                             UploadHandler& handler,
                             resource::INamespacePool& pool,
                             ApiKeyAuth& auth,
                             const deploy::DiskMonitor* disk_monitor = nullptr);

}  // namespace cortrix
