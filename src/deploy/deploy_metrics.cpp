#include "cortrix/deploy/deploy_metrics.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>

namespace cortrix::deploy {

namespace {

// Process-global atomic state. Function-local statics → one set per process,
// zero-init. The build_info labels are immutable after MarkStart/SetBuildInfo, so
// they are guarded by a small mutex only at write time (Render reads a snapshot).
struct State {
    std::atomic<double>  disk_usage_ratio{0.0};
    std::atomic<int>     shutdown_status{0};
    std::atomic<int64_t> start_epoch_sec{0};  // 0 = not marked yet

    std::mutex info_mu;
    std::string version;
    std::string git_commit;
    std::string build_date;
};

State& S() {
    static State s;
    return s;
}

int64_t NowEpochSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Escape a label value for OpenMetrics (backslash, double-quote, newline).
std::string EscapeLabel(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

}  // namespace

DeployMetrics& DeployMetrics::Instance() {
    static DeployMetrics inst;
    return inst;
}

void DeployMetrics::SetDiskUsageRatio(double ratio) {
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    S().disk_usage_ratio.store(ratio, std::memory_order_relaxed);
}

double DeployMetrics::DiskUsageRatio() const {
    return S().disk_usage_ratio.load(std::memory_order_relaxed);
}

void DeployMetrics::SetShutdownStatus(int status) {
    if (status < 0) status = 0;
    if (status > 2) status = 2;
    S().shutdown_status.store(status, std::memory_order_relaxed);
}

int DeployMetrics::ShutdownStatus() const {
    return S().shutdown_status.load(std::memory_order_relaxed);
}

void DeployMetrics::SetBuildInfo(const std::string& version,
                                 const std::string& git_commit,
                                 const std::string& build_date) {
    std::lock_guard<std::mutex> lk(S().info_mu);
    S().version = version;
    S().git_commit = git_commit;
    S().build_date = build_date;
}

void DeployMetrics::MarkStart() {
    S().start_epoch_sec.store(NowEpochSec(), std::memory_order_relaxed);
}

std::string DeployMetrics::Render() const {
    std::ostringstream os;

    // cortrix_disk_usage_ratio (topic 4 — disk management)
    os << "# HELP cortrix_disk_usage_ratio Data-volume usage ratio (used/total), 0..1.\n";
    os << "# TYPE cortrix_disk_usage_ratio gauge\n";
    os << "cortrix_disk_usage_ratio " << S().disk_usage_ratio.load(std::memory_order_relaxed) << "\n";

    // cortrix_shutdown_status (topic 5 — graceful shutdown): 0 = running / 1 = shutting_down / 2 = forced
    os << "# HELP cortrix_shutdown_status 0 = running / 1 = shutting_down / 2 = forced.\n";
    os << "# TYPE cortrix_shutdown_status gauge\n";
    os << "cortrix_shutdown_status " << S().shutdown_status.load(std::memory_order_relaxed) << "\n";

    // cortrix_uptime_seconds
    {
        int64_t start = S().start_epoch_sec.load(std::memory_order_relaxed);
        int64_t uptime = (start == 0) ? 0 : (NowEpochSec() - start);
        if (uptime < 0) uptime = 0;
        os << "# HELP cortrix_uptime_seconds Seconds since process start.\n";
        os << "# TYPE cortrix_uptime_seconds gauge\n";
        os << "cortrix_uptime_seconds " << uptime << "\n";
    }

    // cortrix_build_info{version,git_commit,build_date} (info gauge, value 1)
    {
        std::lock_guard<std::mutex> lk(S().info_mu);
        os << "# HELP cortrix_build_info Build metadata; value is always 1.\n";
        os << "# TYPE cortrix_build_info gauge\n";
        os << "cortrix_build_info{version=\"" << EscapeLabel(S().version)
           << "\",git_commit=\"" << EscapeLabel(S().git_commit)
           << "\",build_date=\"" << EscapeLabel(S().build_date) << "\"} 1\n";
    }

    return os.str();
}

void DeployMetrics::ResetForTest() {
    S().disk_usage_ratio.store(0.0, std::memory_order_relaxed);
    S().shutdown_status.store(0, std::memory_order_relaxed);
    S().start_epoch_sec.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(S().info_mu);
    S().version.clear();
    S().git_commit.clear();
    S().build_date.clear();
}

std::string RenderBloomFilterMetrics(const BloomFilterMetricSource& src) {
    std::ostringstream os;
    const std::string ss = EscapeLabel(src.subsystem);

    if (src.estimated_count) {
        os << "# HELP cortrix_bloom_filter_estimated_count BF current estimated element count.\n";
        os << "# TYPE cortrix_bloom_filter_estimated_count gauge\n";
        os << "cortrix_bloom_filter_estimated_count{subsystem=\"" << ss << "\"} "
           << src.estimated_count() << "\n";
    }
    if (src.false_positive_rate) {
        os << "# HELP cortrix_bloom_filter_false_positive_rate BF runtime false-positive rate.\n";
        os << "# TYPE cortrix_bloom_filter_false_positive_rate gauge\n";
        os << "cortrix_bloom_filter_false_positive_rate{subsystem=\"" << ss << "\"} "
           << src.false_positive_rate() << "\n";
    }
    if (src.last_rebuild_epoch_sec) {
        // §10.1: Unix epoch second. A never-rebuilt filter (-1) emits 0.
        int64_t ts = src.last_rebuild_epoch_sec();
        if (ts < 0) ts = 0;
        os << "# HELP cortrix_bloom_filter_last_rebuild_ts BF last rebuild time (Unix epoch second).\n";
        os << "# TYPE cortrix_bloom_filter_last_rebuild_ts gauge\n";
        os << "cortrix_bloom_filter_last_rebuild_ts{subsystem=\"" << ss << "\"} " << ts << "\n";
    }
    if (src.ready) {
        os << "# HELP cortrix_bloom_filter_ready BF readiness (0 = not ready, 1 = ready).\n";
        os << "# TYPE cortrix_bloom_filter_ready gauge\n";
        os << "cortrix_bloom_filter_ready{subsystem=\"" << ss << "\"} "
           << (src.ready() ? 1 : 0) << "\n";
    }
    return os.str();
}

}  // namespace cortrix::deploy
