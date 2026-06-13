#pragma once
#include <string>

namespace httplib {
class Server;
}

namespace cortrix {

/// F48 §4 (P-3 decision): cortrix-server is the single public endpoint; the
/// agent service stays internal. Registers a reverse proxy
///   /api/v1/agent/<path>  ->  <agent_base_url>/<path>   (SSE streamed for /chat)
///   /agent/<path>         ->  <agent_base_url>/<path>   (P02a settings surface)
/// forwarding Authorization / X-Cortrix-* headers. No-op when agent_base_url
/// is empty (headless deployments without the agent service).
void RegisterAgentProxyRoutes(httplib::Server& server, const std::string& agent_base_url);

}  // namespace cortrix
