#include <cstdint>
#include "cortrix/async/task_cleanup_cron.h"

#include <chrono>
#include <exception>

#include <spdlog/spdlog.h>

#include "cortrix/async/task_metrics.h"

namespace cortrix::async {

namespace {
int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

TaskCleanupCron::TaskCleanupCron(TaskManager* mgr, const IGlobalConfig* config)
    : mgr_(mgr), config_(config) {}

TaskCleanupCron::~TaskCleanupCron() { Stop(); }

// Same UTC-02:00 math as observability::CleanupScheduler::NextRunDelayMs: ms past
// the most recent UTC midnight, then distance forward to 02:00; if already at/past
// 02:00, target tomorrow. Result is always in (0, 24h].
int64_t TaskCleanupCron::NextRunDelayMs(int64_t now_unix_ms) {
    constexpr int64_t kDayMs = 24LL * 3600 * 1000;
    constexpr int64_t kTargetMs = 2LL * 3600 * 1000;  // 02:00
    int64_t ms_into_day = ((now_unix_ms % kDayMs) + kDayMs) % kDayMs;
    int64_t delay = kTargetMs - ms_into_day;
    if (delay <= 0) delay += kDayMs;
    return delay;
}

int TaskCleanupCron::RetentionDays() const {
    if (config_) {
        auto r = config_->GetInt("async.task_retention_days");
        if (r.ok()) return r.value();
    }
    return kDefaultRetentionDays;
}

int TaskCleanupCron::ZombieHours() const {
    if (config_) {
        auto r = config_->GetInt("async.zombie_task_threshold_hours");
        if (r.ok()) return r.value();
    }
    return kDefaultZombieHours;
}

int TaskCleanupCron::TimeoutSeconds() const {
    if (config_) {
        auto r = config_->GetInt("async.task_timeout_seconds");
        if (r.ok()) return r.value();
    }
    return kDefaultTimeoutSeconds;
}

void TaskCleanupCron::RunCleanupNow(int* deleted, int* zombies, int* timed_out) {
    int64_t now_secs = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    // Zombie sweep first (topic 4.3): >zombie_hours with no progress → presumed
    // crashed → failed + ZOMBIE_TASK_CLEANUP.
    auto z = mgr_->SweepZombies(now_secs, ZombieHours());
    int zn = z.ok() ? z.value() : 0;
    // §6.bis cortrix_tasks_zombie_cleaned_total — count the rows this sweep moved
    // processing→failed (bulk SQL sweep carries no per-row task_type, hence no label).
    if (zn > 0) TaskMetrics::Instance().AddZombieCleaned(static_cast<uint64_t>(zn));
    // Then timeout sweep (§6.1 / §7 v0): remaining processing rows that started
    // more than task_timeout_seconds ago exceeded the budget → failed + TASK_TIMEOUT.
    auto t = mgr_->SweepTimedOut(now_secs, TimeoutSeconds());
    int tn = t.ok() ? t.value() : 0;
    auto d = mgr_->DeleteExpired(now_secs, RetentionDays());
    int dn = d.ok() ? d.value() : 0;
    if (zn > 0 || tn > 0 || dn > 0) {
        spdlog::info("tasks cleanup: {} zombies→failed, {} timed-out→failed, "
                     "{} expired deleted", zn, tn, dn);
    }
    if (deleted) *deleted = dn;
    if (zombies) *zombies = zn;
    if (timed_out) *timed_out = tn;
}

void TaskCleanupCron::Start() {
    std::lock_guard<std::mutex> lock(cv_mu_);
    if (started_) return;
    started_ = true;
    stop_ = false;
    thread_ = std::thread([this] { RunLoop(); });
}

void TaskCleanupCron::Stop() {
    {
        std::lock_guard<std::mutex> lock(cv_mu_);
        if (!started_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    started_ = false;
}

void TaskCleanupCron::RunLoop() {
    std::unique_lock<std::mutex> lock(cv_mu_);
    while (!stop_) {
        int64_t wait_ms = test_interval_ms_ > 0 ? test_interval_ms_
                                                : NextRunDelayMs(NowUnixMs());
        cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                     [this] { return stop_; });
        if (stop_) break;
        // Release the cv lock while sweeping (cleanup takes the TaskManager lock).
        lock.unlock();
        // R2-M3 — a sweep failure must not kill this background thread. An
        // exception escaping RunLoop reaches the thread entry point and calls
        // std::terminate(), crashing the whole process; swallow it here and let
        // the loop retry on the next scheduled tick. The catch runs while the cv
        // lock is still released, so we re-acquire it BELOW on every path (caught
        // or not) — keeping the unique_lock's locked/unlocked state consistent
        // with the loop top (unlocking an already-unlocked mutex is UB).
        try {
            RunCleanupNow();
        } catch (const std::exception& e) {
            spdlog::error("tasks cleanup sweep threw: {} — skipped this run, "
                          "will retry next tick", e.what());
        } catch (...) {
            spdlog::error("tasks cleanup sweep threw a non-std exception — "
                          "skipped this run, will retry next tick");
        }
        lock.lock();
    }
}

}  // namespace cortrix::async
