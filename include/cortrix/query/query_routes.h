#pragma once

namespace httplib { class Server; }

namespace cortrix {

class OnnxEmbedder;
class ApiKeyAuth;
class IntentClassifier;
class RRFFusion;

namespace resource { class INamespacePool; }

/// Register query HTTP routes
///
/// Endpoints:
///   POST /api/v1/query  -- unified query endpoint
///
/// @param svr: cpp-httplib server instance
/// @param auth: API Key auth
/// @param pool: NS resource pool (per-request NamespaceFacade Acquire/Release)
/// @param embedder: global OnnxEmbedder
/// @param classifier: global IntentClassifier
/// @param fusion: global RRFFusion
void RegisterQueryRoutes(
    httplib::Server& svr,
    ApiKeyAuth& auth,
    cortrix::resource::INamespacePool& pool,
    OnnxEmbedder& embedder,
    IntentClassifier& classifier,
    RRFFusion& fusion
);

}  // namespace cortrix
