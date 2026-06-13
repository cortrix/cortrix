#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "cortrix/async/task_manager.h"
#include "cortrix/common/i_global_config.h"

namespace cortrix::async {

/// Independent tasks-table cleanup cron (F42 §4.4, topic 4.2 B — decoupled from
/// the OPEN-2 catalog GC). Runs once per day at UTC 02:00 (topic 4.1/4.3):
///   - DeleteExpired: drop completed/failed/cancelled tasks older than
///     task_retention_days (default 30, topic 4.1 B);
///   - SweepZombies: tasks stuck status=processing past
///     zombie_task_threshold_hours (default 24, topic 4.3 B) → failed +
///     CX_ERR_ZOMBIE_TASK_CLEANUP.
///
/// Retention / zombie thresholds are read from IGlobalConfig (f42.* keys, §4.0)
/// on each sweep, so an admin-API hot-reload takes effect on the next run. When a
/// key is missing/malformed the documented default is used.
///
/// Mirrors the observability::CleanupScheduler 02:00 design (same NextRunDelayMs
/// math) but is a single-purpose F42 loop (topic 4.2 — its own cron, not the F18a
/// multi-table scheduler). The wall-clock loop is real and standalone; tests
/// drive RunCleanupNow() and the pure NextRunDelayMs() directly without waiting.
class TaskCleanupCron {
public:
    /// @param mgr    borrowed TaskManager (must outlive this cron)
    /// @param config borrowed IGlobalConfig for f42.task_retention_days /
    ///               f42.zombie_task_threshold_hours (nullptr → use defaults)
    TaskCleanupCron(TaskManager* mgr, const IGlobalConfig* config);
    ~TaskCleanupCron();

    TaskCleanupCron(const TaskCleanupCron&) = delete;
    TaskCleanupCron& operator=(const TaskCleanupCron&) = delete;

    /// Start the daily 02:00 UTC loop (a catch-up sweep is NOT run at start, to
    /// match topic 4.2's "scheduled daily" intent; call RunCleanupNow() explicitly
    /// for a startup sweep). No-op if already started.
    void Start();

    /// Stop and join the background thread. Safe to call twice / without Start.
    void Stop();

    /// Run one full sweep now (SweepZombies + SweepTimedOut + DeleteExpired),
    /// synchronously. Exposed for the scheduled path and for tests. Returns
    /// deleted / zombie / timed-out counts via the out params when non-null.
    void RunCleanupNow(int* deleted = nullptr, int* zombies = nullptr,
                       int* timed_out = nullptr);

    /// Milliseconds from `now_unix_ms` until the next UTC 02:00. Pure / testable;
    /// always in (0, 24h]. (Same contract as CleanupScheduler::NextRunDelayMs.)
    static int64_t NextRunDelayMs(int64_t now_unix_ms);

    /// f42.* default thresholds (§4.0) when config is absent / malformed.
    static constexpr int kDefaultRetentionDays = 30;
    static constexpr int kDefaultZombieHours = 24;
    static constexpr int kDefaultTimeoutSeconds = 1800;  // §4.0 — 30 min

    /// TEST-ONLY: wake every `ms` instead of at 02:00. Set before Start().
    void set_test_interval_ms(int64_t ms) { test_interval_ms_ = ms; }

private:
    void RunLoop();
    int RetentionDays() const;
    int ZombieHours() const;
    int TimeoutSeconds() const;

    TaskManager* mgr_;
    const IGlobalConfig* config_;

    std::thread thread_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool started_ = false;
    int64_t test_interval_ms_ = 0;
};

}  // namespace cortrix::async
