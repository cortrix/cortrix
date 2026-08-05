#pragma once
#include <string>

namespace httplib {
class Server;
}

namespace cortrix {

/// cortrix-server is the single public endpoint; the
/// agent service stays internal. Registers a reverse proxy
///   /api/v1/agent/<path>  ->  <agent_base_url>/<path>   (SSE streamed for /chat)
///   /agent/<path>         ->  <agent_base_url>/<path>   (web UI settings surface)
/// forwarding Authorization / X-Cortrix-* headers. No-op when agent_base_url
/// is empty (headless deployments without the agent service).
void RegisterAgentProxyRoutes(httplib::Server& server, const std::string& agent_base_url);

}  // namespace cortrix
