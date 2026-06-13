#pragma once

namespace httplib { class Server; }

namespace cortrix {
class ApiKeyAuth;
namespace server {
class BatchSubmitService;
}  // namespace server

/// Register the TD-F42-BULK batch submit route (Wave D-R1):
///   POST /api/v1/documents/batch  -- submit 1-100 documents (async, partial-success)
///
/// Write+ auth. The handler parses the JSON body into a server::BatchRequest and
/// delegates to the (borrowed) BatchSubmitService; the service owns all validation
/// + per-doc fan-out + partial-success assembly (transport-free, unit-tested
/// standalone). The service must outlive the server.
void RegisterBatchRoutes(httplib::Server& server,
                         server::BatchSubmitService& service,
                         ApiKeyAuth& auth);

}  // namespace cortrix
