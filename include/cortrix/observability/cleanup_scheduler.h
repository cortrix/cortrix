#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cortrix::observability {

/// Multi-table retention-cleanup scheduler. A shared framework: the operation log
/// registers its table, observability registers agent_trace / interaction_log
/// (both reuse this class). Runs every day at UTC 02:00, plus a
/// catch-up check at StartScheduler() time. Registered cleanups run serially under
/// an in-process advisory lock; a failing table is retried with exponential
/// backoff (max 3 attempts: 1 / 5 / 15 min).
///
/// Standalone (D3): the daily wall-clock loop is real, but cross-Feature wiring
/// (registering the observability tables, the metric emission) is deferred. Tests
/// drive RunCleanupNow() / the pure NextRunDelayMs() directly.
class CleanupScheduler {
public:
    CleanupScheduler() = default;
    ~CleanupScheduler();

    CleanupScheduler(const CleanupScheduler&) = delete;
    CleanupScheduler& operator=(const CleanupScheduler&) = delete;

    /// Register a table's cleanup callback. Idempotent per name is NOT enforced;
    /// the caller registers each table once at startup. `cleanup_fn` must not
    /// throw (it is invoked under the scheduler lock); the operation log passes
    /// [logger]{ logger->Cleanup(); }.
    void RegisterTable(const std::string& table_name,
                       std::function<void()> cleanup_fn);

    /// Start the background thread: run a catch-up sweep now, then wake at each
    /// UTC 02:00. No-op if already started.
    void StartScheduler();

    /// Stop the background thread (joins). Safe to call twice / without Start.
    void StopScheduler();

    /// Run every registered table's cleanup once, synchronously, with retry/backoff.
    /// Exposed for the catch-up path and for tests. Honors the advisory lock so a
    /// manual call can't overlap the scheduled sweep.
    void RunCleanupNow();

    /// Number of registered tables (test aid).
    size_t registered_count() const;

    /// True while a sweep is in progress (the advisory-lock state; §8.1).
    bool is_running() const { return running_.load(); }

    /// Milliseconds from `now_unix_ms` until the next UTC 02:00 (pure; testable).
    /// Always in (0, 24h]: if it is exactly 02:00 the next run is 24h out.
    static int64_t NextRunDelayMs(int64_t now_unix_ms);

    /// Backoff for attempt N (1-based): 60s / 300s / 900s (topic 5). Returns 0 for
    /// attempts beyond the 3rd (caller stops retrying).
    static int64_t BackoffMs(int attempt);

    /// Max retry attempts per table per sweep (topic 5).
    static constexpr int kMaxAttempts = 3;

private:
    struct Table {
        std::string name;
        std::function<void()> fn;
    };

    void RunLoop();
    /// Run one table's cleanup with up to kMaxAttempts attempts. Returns true if
    /// it ultimately succeeded. `sleep_fn` lets tests inject a no-wait backoff.
    bool RunOneWithRetry(const Table& t);

    std::vector<Table> tables_;
    mutable std::mutex tables_mu_;        ///< guards tables_ registration

    std::mutex run_mu_;                   ///< advisory lock — one sweep at a time
    std::atomic<bool> running_{false};    ///< sweep-in-progress flag

    std::thread thread_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool started_ = false;

    // Test seam: when > 0, the loop waits this many ms instead of NextRunDelayMs()
    // (so a test can exercise the loop without waiting for 02:00). 0 = real clock.
    int64_t test_interval_ms_ = 0;

public:
    /// TEST-ONLY: make the loop wake every `ms` instead of at UTC 02:00. Set
    /// before StartScheduler(). Not used in production wiring.
    void set_test_interval_ms(int64_t ms) { test_interval_ms_ = ms; }
};

}  // namespace cortrix::observability
