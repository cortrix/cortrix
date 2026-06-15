#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cortrix/deploy/deploy_metrics.h"

namespace httplib { class Server; }

namespace cortrix::deploy {

/// OpenMetrics endpoint server (F24 §5, F24-3 decision B — a single, separate
/// metrics port + anonymous access). Runs an independent httplib::Server on
/// :9091 serving `GET /metrics` in OpenMetrics text exposition. Separate from the
/// main :8420 API server so the metrics port can be kept inside the network
/// (K8s ServiceMonitor / Prometheus scrape) while the API is public.
///
///   GET /metrics
///     → 200 OK, Content-Type: text/plain; version=0.0.4; charset=utf-8
///     → anonymous (no auth — port-level isolation is the boundary, §5.2)
///     → body = concatenation of every registered metric source's render
///
/// Sources: the DeployMetrics gauges (disk_usage_ratio / shutdown_status /
/// uptime / build_info) + the read-through Bloom Filter gauges (§10) + any extra
/// renderers added via AddSource(). The remaining §5.3 subsystem recorders
/// (F42Metrics, ScoringMetrics, ...) are aggregated by AddSource() at D3.5 wiring
/// time — the server is the §5 piece; collecting all 25 metrics into one body is
/// the cross-Feature wiring step.
class MetricsServer {
public:
    /// A metric text producer (each returns its own OpenMetrics block).
    using MetricSource = std::function<std::string()>;

    /// @param port  listen port (default 9091, F24-3 / OBS_SPEC §5.1)
    /// @param bind  bind address (default 0.0.0.0 — port-level isolation, §5.2)
    explicit MetricsServer(int port = 9091, std::string bind = "0.0.0.0");
    ~MetricsServer();

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    /// Register a metric text source (appended to the /metrics body in order).
    /// The DeployMetrics gauges are added automatically in the constructor.
    void AddSource(MetricSource source);

    /// Bind the read-through Bloom Filter gauges (§10) from a source struct.
    /// Convenience over AddSource(RenderBloomFilterMetrics-bound).
    void AddBloomFilterSource(BloomFilterMetricSource src);

    /// Render the full /metrics body now (every source concatenated). Exposed so
    /// the body is unit-testable without binding a socket.
    std::string RenderAll() const;

    /// Start listening on a background thread (idempotent). Returns false if the
    /// bind fails (port in use). The handler is registered before listening.
    bool Start();

    /// Stop the server and join the thread (idempotent).
    void Stop();

    int port() const { return port_; }

    /// The fixed content type for the /metrics response (OBS_SPEC §5.2).
    static constexpr const char* kContentType =
        "text/plain; version=0.0.4; charset=utf-8";

private:
    void RegisterRoute();

    int port_;
    std::string bind_;
    std::vector<MetricSource> sources_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

/// Convenience: build + start a MetricsServer wired to the DeployMetrics gauges
/// (and `bf` if non-null). Returns the running server (caller keeps it alive;
/// destroying it stops the thread). main.cpp calls this — the briefing's
/// `RegisterMetricsServer()` entry point. Returns nullptr if the bind fails.
std::unique_ptr<MetricsServer> RegisterMetricsServer(
    int port = 9091,
    BloomFilterMetricSource bf = {});

}  // namespace cortrix::deploy
