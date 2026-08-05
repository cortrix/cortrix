#include "cortrix/deploy/health_routes.h"

#include "httplib.h"
#include <chrono>
#include <nlohmann/json.hpp>

namespace cortrix::deploy {

void RegisterHealthRoutes(httplib::Server& server,
                          const health::ReadinessRegistry& registry,
                          std::function<bool()> is_shutting_down,
                          std::string version) {
    // GET /api/v1/system/health/live — liveness. The process answers, so it is
    // alive; this only fails by virtue of the listener being down (handled by
    // the proxy/orchestrator timing out). Always 200 here. Carries
    // uptime_seconds + version like /health does (the the web UI HealthPage reads
    // both fields from this probe).
    const auto start_time = std::chrono::steady_clock::now();

    // GET /api/v1/system/version — public version info (issue ⑭ · openapi
    // system.yaml getSystemVersion). No auth. `build` / `commit` are reserved
    // (empty in V1.0; SetBuildInfo feeds the metric, not this body) and omitted
    // when empty so the {version} required field is the only guaranteed key.
    server.Get("/api/v1/system/version",
        [version](const httplib::Request&, httplib::Response& res) {
            nlohmann::json body;
            body["version"] = version;
            res.set_content(body.dump(), "application/json");
            res.status = 200;
        });

    server.Get("/api/v1/system/health/live",
        [start_time, version](const httplib::Request&, httplib::Response& res) {
            nlohmann::json body;
            body["status"] = "alive";
            body["uptime_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            body["version"] = version;
            res.set_content(body.dump(), "application/json");
            res.status = 200;
        });

    // GET /api/v1/system/health/ready — readiness. Aggregates the
    // ReadinessRegistry (every registered component's CheckReady()) via
    // BuildReport(), then applies the shutdown gate: 200 only when NOT shutting
    // down AND all components are ready; otherwise 503 listing the failures (so
    // K8s/compose drop traffic without restarting during start_period).
    server.Get("/api/v1/system/health/ready",
        [&registry, is_shutting_down, version](const httplib::Request&, httplib::Response& res) {
            bool all_ready = false;
            nlohmann::json body = registry.BuildReport(&all_ready);

            // Shutdown gate: a draining process must report not-ready even if every
            // component still probes ready, so the orchestrator stops routing to it.
            if (is_shutting_down && is_shutting_down()) {
                all_ready = false;
                body["status"] = "not_ready";
                body["components"]["shutdown"] = {{"status", "shutting_down"}};
            }
            if (!version.empty()) {
                body["version"] = version;
            }

            res.set_content(body.dump(), "application/json");
            res.status = all_ready ? 200 : 503;
        });
}

}  // namespace cortrix::deploy
