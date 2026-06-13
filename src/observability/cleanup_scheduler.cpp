#include "cortrix/observability/cleanup_scheduler.h"

#include <chrono>
#include <utility>

namespace cortrix::observability {

namespace {
constexpr int64_t kMsPerDay = 86400000LL;
constexpr int64_t kUtcCleanupHour = 2;  // topic 5 — UTC 02:00
}  // namespace

CleanupScheduler::~CleanupScheduler() { StopScheduler(); }

void CleanupScheduler::RegisterTable(const std::string& table_name,
                                     std::function<void()> cleanup_fn) {
    if (!cleanup_fn) return;
    std::lock_guard<std::mutex> lock(tables_mu_);
    tables_.push_back({table_name, std::move(cleanup_fn)});
}

size_t CleanupScheduler::registered_count() const {
    std::lock_guard<std::mutex> lock(tables_mu_);
    return tables_.size();
}

int64_t CleanupScheduler::NextRunDelayMs(int64_t now_unix_ms) {
    // ms since the start of the current UTC day.
    const int64_t ms_into_day = ((now_unix_ms % kMsPerDay) + kMsPerDay) % kMsPerDay;
    const int64_t target = kUtcCleanupHour * 3600000LL;  // 02:00 in ms-of-day
    int64_t delay = target - ms_into_day;
    if (delay <= 0) delay += kMsPerDay;  // already past 02:00 → tomorrow's 02:00
    return delay;
}

int64_t CleanupScheduler::BackoffMs(int attempt) {
    switch (attempt) {
        case 1: return 60000;    // 1 min
        case 2: return 300000;   // 5 min
        case 3: return 900000;   // 15 min
        default: return 0;       // no more retries
    }
}

bool CleanupScheduler::RunOneWithRetry(const Table& t) {
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        try {
            t.fn();
            return true;  // cleanup callbacks are no-throw on success
        } catch (...) {
            // A throwing cleanup is treated as a failed attempt. Back off, unless
            // this was the last attempt. The wait is interruptible by StopScheduler.
            const int64_t backoff = BackoffMs(attempt);
            if (attempt == kMaxAttempts || backoff == 0) return false;
            std::unique_lock<std::mutex> lk(cv_mu_);
            if (cv_.wait_for(lk, std::chrono::milliseconds(backoff),
                             [this] { return stop_; })) {
                return false;  // stopping — abandon retries
            }
        }
    }
    return false;
}

void CleanupScheduler::RunCleanupNow() {
    // Advisory lock: only one sweep at a time (topic 5). try_lock so an overlapping
    // call returns immediately rather than queueing.
    std::unique_lock<std::mutex> sweep(run_mu_, std::try_to_lock);
    if (!sweep.owns_lock()) return;
    running_.store(true);

    std::vector<Table> snapshot;
    {
        std::lock_guard<std::mutex> lock(tables_mu_);
        snapshot = tables_;
    }
    for (const auto& t : snapshot) {
        RunOneWithRetry(t);
    }
    running_.store(false);
}

void CleanupScheduler::StartScheduler() {
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        if (started_) return;
        started_ = true;
        stop_ = false;
    }
    thread_ = std::thread([this] { RunLoop(); });
}

void CleanupScheduler::StopScheduler() {
    // Always raise the stop signal + notify, even if the background loop was never
    // started: a RunCleanupNow() invoked directly (e.g. the catch-up path or a
    // test) may be parked in an inter-attempt backoff wait, and `stop_` is the
    // shared interrupt for those waits. Joining is conditional on a live thread.
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(cv_mu_);
    started_ = false;
}

void CleanupScheduler::RunLoop() {
    // Catch-up sweep on startup (§8.1 "startup check").
    RunCleanupNow();

    for (;;) {
        int64_t wait_ms;
        if (test_interval_ms_ > 0) {
            wait_ms = test_interval_ms_;
        } else {
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            wait_ms = NextRunDelayMs(now);
        }
        std::unique_lock<std::mutex> lk(cv_mu_);
        if (cv_.wait_for(lk, std::chrono::milliseconds(wait_ms),
                         [this] { return stop_; })) {
            return;  // stop requested
        }
        lk.unlock();
        RunCleanupNow();
    }
}

}  // namespace cortrix::observability
