#pragma once
#include <functional>

namespace cortrix {

struct CortrixConfig;
class ApiKeyAuth;
class SPCManager;
class CortrixHttpServer;
namespace resource { class INamespacePool; }

namespace server {

/// Assembly context handed to embedder extension hooks. All references stay
/// valid from on_assembled until RunServer returns (i.e. through shutdown).
struct ServerContext {
    CortrixHttpServer& http;
    CortrixConfig& config;
    resource::INamespacePool& ns_pool;
    SPCManager& spc;
    ApiKeyAuth& auth;
};

/// Optional hooks for embedders that assemble extra modules (routes,
/// background services) on top of the stock server. The stock cortrix-server
/// binary passes none — RunServer behaves identically with the default.
struct ServerExtensions {
    /// Invoked once after all core routes/services are assembled, before the
    /// listener starts. Register additional routes / start embedder services.
    std::function<void(ServerContext&)> on_assembled;
    /// Invoked during ordered shutdown, after the listener stops and before
    /// core services tear down. Stop embedder services here.
    std::function<void()> on_shutdown;
};

/// Full server bootstrap: config load, assembly, signal handling, run loop and
/// graceful shutdown — the entire former main(). Returns the process exit code.
int RunServer(int argc, char* argv[], const ServerExtensions& extensions = {});

}  // namespace server
}  // namespace cortrix
