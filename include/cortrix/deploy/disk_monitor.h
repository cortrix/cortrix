#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "cortrix/common/i_global_config.h"
#include "cortrix/deploy/f24_error.h"

namespace cortrix::deploy {

/// Disk pressure stage (F24 §6.1, F24-4 decision A — active degradation + write
/// rejection). The three thresholds map ratio → stage:
///   NORMAL  ratio < warn   : normal operation
///   WARN    ratio >= warn   : log_warn + metric gauge update (writes still allowed)
///   CRIT    ratio >= crit   : reject all new writes + CX_ERR_DISK_FULL
enum class DiskStage {
    kNormal = 0,
    kWarn   = 1,
    kCrit   = 2,
};

const char* ToString(DiskStage stage);

/// Snapshot of a single statvfs sample. usage_ratio = used / total.
struct DiskUsage {
    uint64_t total_bytes = 0;
    uint64_t free_bytes  = 0;       ///< available to an unprivileged process (f_bavail)
    double   usage_ratio = 0.0;     ///< (total - free) / total, clamped to [0,1]
    DiskStage stage = DiskStage::kNormal;
};

/// The IGlobalConfig keys + documented defaults the monitor reads.
/// Generic KV accessors (IGlobalConfig::GetFloat/GetInt) keep the canonical
/// config header dependency-free; an admin hot-reload is picked up on the
/// next check tick. Standalone (D3): real + tested against InMemoryGlobalConfig.
struct DiskMonitorConfig {
    std::string data_dir = "./data";   ///< path passed to statvfs (CORTRIX_DATA_DIR)
    double warn_threshold = 0.80;      ///< key "disk_warn_threshold"      (range 0.5–0.95)
    double crit_threshold = 0.90;      ///< key "disk_crit_threshold"      (range 0.6–0.99)
    int    check_interval_sec = 30;    ///< key "disk_check_interval_sec"  (range 10–300)

    static constexpr const char* kWarnKey     = "disk_warn_threshold";
    static constexpr const char* kCritKey     = "disk_crit_threshold";
    static constexpr const char* kIntervalKey = "disk_check_interval_sec";

    /// Read the three keys from `config` over the defaults above (each key absent
    /// → keep the default). Out-of-range values are clamped to the §6.4 ranges.
    /// `config` may be nullptr (all defaults). `data_dir` is set by the caller.
    static DiskMonitorConfig FromGlobalConfig(const IGlobalConfig* config,
                                              const std::string& data_dir);
};

/// Periodic disk-space watchdog (F24 §6, F24-4 decision A). Owns a background
/// thread that statvfs(data_dir) every check_interval_sec, computes the usage
/// ratio, transitions NORMAL/WARN/CRIT, and flips an atomic `reject_new_writes`
/// flag the SPC write path consults before admitting a new write.
///
/// Standalone (D3): the monitor + CheckOnce() are fully usable and tested
/// in-process (CheckOnce drives the FSM deterministically; the thread is just a
/// scheduler around it). Wiring the gauge into the F24 /metrics endpoint and the
/// flag into the real SPC pipeline is in this Feature; cross-Feature E2E is D3.5.
///
/// Thread-safe: ShouldRejectWrites() / Usage() are lock-free (atomics); Start /
/// Stop serialize on the internal mutex.
class DiskMonitor {
public:
    /// Optional sink invoked on every transition into a *different* stage (for a
    /// log line / metric side effect). Receives the new sample. May be null.
    using StageChangeCallback = std::function<void(const DiskUsage&)>;

    explicit DiskMonitor(DiskMonitorConfig config,
                         StageChangeCallback on_stage_change = nullptr);
    ~DiskMonitor();

    DiskMonitor(const DiskMonitor&) = delete;
    DiskMonitor& operator=(const DiskMonitor&) = delete;

    /// Run one statvfs sample synchronously: update usage_, stage_, the reject
    /// flag, and (on a stage change) fire the callback. Returns the new sample.
    /// This is the unit-testable core; the background thread just calls it on a
    /// timer. A statvfs failure leaves the previous state untouched and returns
    /// the last sample (fail-safe: never falsely rejects on a transient stat error).
    DiskUsage CheckOnce();

    /// Start the background check loop (idempotent — a second call is a no-op).
    void Start();

    /// Stop the background loop and join the thread (idempotent).
    void Stop();

    /// True once usage_ratio >= crit_threshold — the SPC write path rejects new
    /// writes (returns CX_ERR_DISK_FULL) while this holds. Lock-free.
    bool ShouldRejectWrites() const { return reject_new_writes_.load(std::memory_order_relaxed); }

    /// Test seam: apply the configured thresholds to an injected raw sample
    /// (total_bytes/free_bytes/usage_ratio) instead of calling statvfs, then
    /// commit it exactly as CheckOnce() would (atomics + reject flag + metric +
    /// stage-change callback). Lets unit tests drive the NORMAL/WARN/CRIT FSM
    /// deterministically. Returns the classified sample.
    DiskUsage CheckWithSample(DiskUsage raw);

    /// Last sampled usage (lock-free copy of the atomics).
    DiskUsage Usage() const;

    /// Build the CX_ERR_DISK_FULL Agent-friendly error body for the current
    /// sample (§6.3 — fills the 5 required structured_data keys from `cfg`/usage).
    agent_friendly::AgentFriendlyError MakeDiskFullError() const;

    const DiskMonitorConfig& config() const { return config_; }

private:
    void Loop();
    void Apply(const DiskUsage& sample);  // commit a sample to the atomics + callback

    DiskMonitorConfig config_;
    StageChangeCallback on_stage_change_;

    std::atomic<bool> reject_new_writes_{false};
    // usage snapshot, stored as atomics for lock-free reads:
    std::atomic<uint64_t> total_bytes_{0};
    std::atomic<uint64_t> free_bytes_{0};
    std::atomic<int>      stage_{static_cast<int>(DiskStage::kNormal)};
    // usage_ratio is reconstructed from total/free on read to avoid a double atomic.

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
};

/// statvfs `data_dir` and compute a DiskUsage (no thresholds applied — stage is
/// left kNormal). Exposed for tests + callers that want a one-off sample.
/// Returns false (and leaves *out untouched) if statvfs fails.
bool SampleDiskUsage(const std::string& data_dir, DiskUsage* out);

}  // namespace cortrix::deploy
