#include "cortrix/deploy/metrics_server.h"

#include "httplib.h"

#include <sstream>
#include <utility>

namespace cortrix::deploy {

MetricsServer::MetricsServer(int port, std::string bind)
    : port_(port), bind_(std::move(bind)), svr_(std::make_unique<httplib::Server>()) {
    // The DeployMetrics gauges are always part of the body (disk / shutdown /
    // uptime / build_info). Captured as a source so RenderAll() reflects live values.
    sources_.push_back([] { return DeployMetrics::Instance().Render(); });
}

MetricsServer::~MetricsServer() {
    Stop();
}

void MetricsServer::AddSource(MetricSource source) {
    if (source) sources_.push_back(std::move(source));
}

void MetricsServer::AddBloomFilterSource(BloomFilterMetricSource src) {
    sources_.push_back([src = std::move(src)] { return RenderBloomFilterMetrics(src); });
}

std::string MetricsServer::RenderAll() const {
    std::ostringstream os;
    for (const auto& src : sources_) {
        os << src();
    }
    return os.str();
}

void MetricsServer::RegisterRoute() {
    svr_->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(RenderAll(), kContentType);
        res.status = 200;
    });
}

bool MetricsServer::Start() {
    if (running_.load()) return true;
    RegisterRoute();

    // Probe-bind on this thread so a port clash is reported synchronously, then
    // hand the listening loop to the background thread.
    if (!svr_->bind_to_port(bind_.c_str(), port_)) {
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { svr_->listen_after_bind(); });
    return true;
}

void MetricsServer::Stop() {
    if (!running_.exchange(false)) return;
    if (svr_) svr_->stop();
    if (thread_.joinable()) thread_.join();
}

std::unique_ptr<MetricsServer> RegisterMetricsServer(int port,
                                                     BloomFilterMetricSource bf) {
    auto server = std::make_unique<MetricsServer>(port);
    // Only add the BF source if at least one accessor is bound (otherwise the
    // gauges would render nothing useful and just add noise).
    if (bf.estimated_count || bf.false_positive_rate || bf.last_rebuild_epoch_sec ||
        bf.ready) {
        server->AddBloomFilterSource(std::move(bf));
    }
    if (!server->Start()) {
        return nullptr;  // bind failed (port in use)
    }
    return server;
}

}  // namespace cortrix::deploy
