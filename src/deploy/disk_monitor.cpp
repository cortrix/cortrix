#include <cstdint>
#include "cortrix/deploy/disk_monitor.h"

#include <sys/statvfs.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

// Whole-string parses for the environment variables: surrounding ASCII blanks
// are allowed, nothing else is. A partial parse ("60s", "0.8x") is a typo, not a
// value, so it is rejected instead of being read as 60 or 0.8.
const char* SkipBlanks(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

bool ParseWholeDouble(const char* text, double* out) {
    const char* p = SkipBlanks(text);
    if (*p == '\0') return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(p, &end);
    if (end == p || errno == ERANGE || !std::isfinite(v)) return false;
    if (*SkipBlanks(end) != '\0') return false;
    *out = v;
    return true;
}

bool ParseWholeInt(const char* text, int* out) {
    const char* p = SkipBlanks(text);
    if (*p == '\0') return false;
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(p, &end, 10);
    if (end == p || errno == ERANGE || v < INT_MIN || v > INT_MAX) return false;
    if (*SkipBlanks(end) != '\0') return false;
    *out = static_cast<int>(v);
    return true;
}

std::string Two(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
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

DiskMonitorConfig DiskMonitorConfig::FromEnvironment(const std::string& data_dir,
                                                     std::vector<std::string>* notes) {
    DiskMonitorConfig c;
    c.data_dir = data_dir;
    auto note = [notes](std::string text) {
        if (notes) notes->push_back(std::move(text));
    };
    // Unset or empty keeps the default silently: that is the documented way of
    // saying "the default", and it is what every deployment without the
    // variables has always done.
    auto read_ratio = [&](const char* env, double lo, double hi, double* field) {
        const char* raw = std::getenv(env);
        if (!raw || !*raw) return;
        double v = 0.0;
        if (!ParseWholeDouble(raw, &v)) {
            note(std::string(env) + "=\"" + raw + "\" is not a number; keeping " + Two(*field));
            return;
        }
        const double clamped = Clamp(v, lo, hi);
        if (clamped != v) {
            note(std::string(env) + "=" + Two(v) + " is outside " + Two(lo) + "-" + Two(hi) +
                 "; clamped to " + Two(clamped));
        }
        *field = clamped;
    };
    read_ratio(kWarnEnv, 0.5, 0.95, &c.warn_threshold);
    read_ratio(kCritEnv, 0.6, 0.99, &c.crit_threshold);
    if (const char* raw = std::getenv(kIntervalEnv); raw && *raw) {
        int v = 0;
        if (!ParseWholeInt(raw, &v)) {
            note(std::string(kIntervalEnv) + "=\"" + raw + "\" is not a whole number of seconds; keeping " +
                 std::to_string(c.check_interval_sec));
        } else {
            const int clamped = std::min(std::max(v, 10), 300);
            if (clamped != v) {
                note(std::string(kIntervalEnv) + "=" + std::to_string(v) +
                     " is outside 10-300; clamped to " + std::to_string(clamped));
            }
            c.check_interval_sec = clamped;
        }
    }
    // Guard against a misconfigured warn >= crit: keep warn strictly below crit so
    // the WARN band exists (clamp warn down to crit if inverted).
    if (c.warn_threshold >= c.crit_threshold) {
        const double before = c.warn_threshold;
        c.warn_threshold = std::max(0.5, c.crit_threshold - 0.01);
        note("warn threshold " + Two(before) + " is not below crit threshold " +
             Two(c.crit_threshold) + "; warn lowered to " + Two(c.warn_threshold));
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
