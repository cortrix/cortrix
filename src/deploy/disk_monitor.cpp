#include <cstdint>
#include "cortrix/deploy/disk_monitor.h"

#include <sys/statvfs.h>

#include <algorithm>
#include <chrono>

#include "cortrix/deploy/deploy_metrics.h"

namespace cortrix::deploy {

const char* ToString(DiskStage stage) {
    switch (stage) {
        case DiskStage::kNormal: return "normal";
        case DiskStage::kWarn:   return "warn";
        case DiskStage::kCrit:   return "crit";
    }
    return "normal";
}

namespace {

double Clamp(double v, double lo, double hi) {
    return std::min(std::max(v, lo), hi);
}

DiskStage StageFor(double ratio, double warn, double crit) {
    if (ratio >= crit) return DiskStage::kCrit;
    if (ratio >= warn) return DiskStage::kWarn;
    return DiskStage::kNormal;
}

}  // namespace

bool SampleDiskUsage(const std::string& data_dir, DiskUsage* out) {
    struct statvfs vfs;
    if (statvfs(data_dir.c_str(), &vfs) != 0) {
        return false;
    }
    // Use f_frsize (fragment size) for byte math, the portable choice; fall back
    // to f_bsize if a platform reports f_frsize == 0.
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    uint64_t total = static_cast<uint64_t>(vfs.f_blocks) * unit;
    // f_bavail = blocks available to an unprivileged process (what a write can use).
    uint64_t avail = static_cast<uint64_t>(vfs.f_bavail) * unit;
    DiskUsage u;
    u.total_bytes = total;
    u.free_bytes = avail;
    u.usage_ratio = (total == 0) ? 0.0 : Clamp(1.0 - static_cast<double>(avail) / static_cast<double>(total), 0.0, 1.0);
    u.stage = DiskStage::kNormal;  // thresholds applied by the caller
    *out = u;
    return true;
}

DiskMonitorConfig DiskMonitorConfig::FromGlobalConfig(const IGlobalConfig* config,
                                                      const std::string& data_dir) {
    DiskMonitorConfig c;
    c.data_dir = data_dir;
    if (config) {
        if (auto r = config->GetFloat(kWarnKey); r.ok()) {
            c.warn_threshold = Clamp(static_cast<double>(*r), 0.5, 0.95);
        }
        if (auto r = config->GetFloat(kCritKey); r.ok()) {
            c.crit_threshold = Clamp(static_cast<double>(*r), 0.6, 0.99);
        }
        if (auto r = config->GetInt(kIntervalKey); r.ok()) {
            c.check_interval_sec = static_cast<int>(Clamp(static_cast<double>(*r), 10, 300));
        }
    }
    // Guard against a misconfigured warn >= crit: keep warn strictly below crit so
    // the WARN band exists (clamp warn down to crit if inverted).
    if (c.warn_threshold >= c.crit_threshold) {
        c.warn_threshold = std::max(0.5, c.crit_threshold - 0.01);
    }
    return c;
}

DiskMonitor::DiskMonitor(DiskMonitorConfig config, StageChangeCallback on_stage_change)
    : config_(std::move(config)), on_stage_change_(std::move(on_stage_change)) {}

DiskMonitor::~DiskMonitor() {
    Stop();
}

void DiskMonitor::Apply(const DiskUsage& sample) {
    int prev_stage = stage_.exchange(static_cast<int>(sample.stage), std::memory_order_relaxed);
    total_bytes_.store(sample.total_bytes, std::memory_order_relaxed);
    free_bytes_.store(sample.free_bytes, std::memory_order_relaxed);
    // CRIT → reject new writes; NORMAL/WARN → allow (F24-4 decision A).
    reject_new_writes_.store(sample.stage == DiskStage::kCrit, std::memory_order_relaxed);

    // Update the gauge on every sample (§6.1 "metric gauge update").
    DeployMetrics::Instance().SetDiskUsageRatio(sample.usage_ratio);

    // Fire the callback only when the stage actually changed (log throttling).
    if (prev_stage != static_cast<int>(sample.stage) && on_stage_change_) {
        on_stage_change_(sample);
    }
}

DiskUsage DiskMonitor::CheckOnce() {
    DiskUsage sample;
    if (!SampleDiskUsage(config_.data_dir, &sample)) {
        // Fail-safe: a transient statvfs error must not flip the reject flag.
        // Return the last known sample unchanged.
        return Usage();
    }
    sample.stage = StageFor(sample.usage_ratio, config_.warn_threshold, config_.crit_threshold);
    Apply(sample);
    return sample;
}

DiskUsage DiskMonitor::CheckWithSample(DiskUsage raw) {
    raw.stage = StageFor(raw.usage_ratio, config_.warn_threshold, config_.crit_threshold);
    Apply(raw);
    return raw;
}

void DiskMonitor::Start() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
}

void DiskMonitor::Stop() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DiskMonitor::Loop() {
    // Sample immediately on start, then every check_interval_sec until stopped.
    CheckOnce();
    std::unique_lock<std::mutex> lk(mutex_);
    while (running_) {
        cv_.wait_for(lk, std::chrono::seconds(config_.check_interval_sec),
                     [this] { return !running_; });
        if (!running_) break;
        lk.unlock();
        CheckOnce();
        lk.lock();
    }
}

DiskUsage DiskMonitor::Usage() const {
    DiskUsage u;
    u.total_bytes = total_bytes_.load(std::memory_order_relaxed);
    u.free_bytes = free_bytes_.load(std::memory_order_relaxed);
    u.usage_ratio = (u.total_bytes == 0)
                        ? 0.0
                        : Clamp(1.0 - static_cast<double>(u.free_bytes) /
                                          static_cast<double>(u.total_bytes),
                                0.0, 1.0);
    u.stage = static_cast<DiskStage>(stage_.load(std::memory_order_relaxed));
    return u;
}

agent_friendly::AgentFriendlyError DiskMonitor::MakeDiskFullError() const {
    DiskUsage u = Usage();
    nlohmann::json sd = {
        {"disk_usage_ratio", u.usage_ratio},
        {"free_bytes", u.free_bytes},
        {"data_path", config_.data_dir},
        {"warn_threshold", config_.warn_threshold},
        {"crit_threshold", config_.crit_threshold},
    };
    return MakeF24Error(F24ErrorCode::kDiskFull, std::move(sd),
                        "Disk usage critical (>= crit threshold). Operations rejected.");
}

}  // namespace cortrix::deploy
