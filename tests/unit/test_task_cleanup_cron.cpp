#include <gtest/gtest.h>

#include <chrono>

#include "cortrix/async/task_cleanup_cron.h"
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/common/in_memory_global_config.h"

// S1 coverage: TaskCleanupCron (Issue 4.2 independent 02:00 cron) — the pure
// NextRunDelayMs scheduling math, the config-driven thresholds, and RunCleanupNow
// wiring DeleteExpired + SweepZombies over a real TaskManager.
namespace cortrix::async {
namespace {

TaskInfo MakeTask(const std::string& doc) {
    TaskInfo t;
    t.namespace_id = "ns1";
    t.filename = "f.pdf";
    t.filepath = "/tmp/f.pdf";
    t.doc_id = doc;
    t.content_hash = "h";
    return t;
}

// ---- NextRunDelayMs (pure, UTC 02:00) — same contract as CleanupScheduler ----

TEST(TaskCleanupCronTimeTest, NextRunDelayWithinADay) {
    // 1970-01-01 00:00:00 UTC → next 02:00 is +2h.
    EXPECT_EQ(TaskCleanupCron::NextRunDelayMs(0), 2LL * 3600000);
    // exactly 02:00 → next is +24h (never 0).
    EXPECT_EQ(TaskCleanupCron::NextRunDelayMs(2LL * 3600000), 24LL * 3600000);
    // 03:00 → 23h to tomorrow's 02:00.
    EXPECT_EQ(TaskCleanupCron::NextRunDelayMs(3LL * 3600000), 23LL * 3600000);
    // 01:59:59 → 1s away.
    EXPECT_EQ(TaskCleanupCron::NextRunDelayMs(2LL * 3600000 - 1000), 1000);
}

TEST(TaskCleanupCronTimeTest, NextRunDelayArbitraryEpochInRange) {
    int64_t now = 1747584000000LL;  // 2025-05-18ish
    int64_t d = TaskCleanupCron::NextRunDelayMs(now);
    EXPECT_GT(d, 0);
    EXPECT_LE(d, 24LL * 3600000);
}

// ---- RunCleanupNow over a real TaskManager --------------------------------

TEST(TaskCleanupCronRunTest, RunCleanupNowSweepsZombiesAndDeletesExpired) {
    TaskManager mgr;
    ASSERT_TRUE(mgr.Init(":memory:").ok());

    // one completed (will expire), one stale processing (zombie).
    auto done = mgr.CreateTask(MakeTask("d-done"));
    mgr.MarkCompleted(done.value().task_id, "doc-done");
    auto zomb = mgr.CreateTask(MakeTask("d-zomb"));
    mgr.MarkProcessing(zomb.value().task_id, 1);

    // RunCleanupNow reads the real wall clock; the cutoffs are strict (updated_at
    // < now - threshold). With threshold 0 the cutoff == now, so we let ~1.1s
    // elapse to push the rows' updated_at strictly before "now" on the sweep.
    InMemoryGlobalConfig cfg;
    cfg.Set("async.task_retention_days", "0");          // delete terminal older than now
    cfg.Set("async.zombie_task_threshold_hours", "0");  // processing older than now = zombie
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    TaskCleanupCron cron(&mgr, &cfg);
    int deleted = -1, zombies = -1;
    cron.RunCleanupNow(&deleted, &zombies);
    EXPECT_EQ(zombies, 1);  // processing → failed
    // After the zombie flip the row is failed (terminal); with retention_days=0
    // and updated_at < now it is also deleted in the same sweep, along with the
    // already-completed one.
    EXPECT_GE(deleted, 1);
}

TEST(TaskCleanupCronConfigTest, FallsBackToDefaultsWhenConfigMissing) {
    TaskManager mgr;
    ASSERT_TRUE(mgr.Init(":memory:").ok());
    auto p = mgr.CreateTask(MakeTask("d1"));
    mgr.MarkProcessing(p.value().task_id, 1);

    // No config → defaults (30d / 24h). A fresh processing task is NOT a zombie.
    TaskCleanupCron cron(&mgr, nullptr);
    int deleted = -1, zombies = -1;
    cron.RunCleanupNow(&deleted, &zombies);
    EXPECT_EQ(zombies, 0);
    EXPECT_EQ(deleted, 0);
    EXPECT_EQ(mgr.GetTask(p.value().task_id).value().status,
              std::string(task_status::kProcessing));
}

TEST(TaskCleanupCronRunTest, StartStopIsSafeAndIdempotent) {
    TaskManager mgr;
    ASSERT_TRUE(mgr.Init(":memory:").ok());
    TaskCleanupCron cron(&mgr, nullptr);
    cron.set_test_interval_ms(10);  // wake fast instead of waiting for 02:00
    cron.Start();
    cron.Start();  // no-op second start
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    cron.Stop();
    cron.Stop();  // safe double stop
    SUCCEED();
}

// RunCleanupNow with the timed_out out-param populated + a real timeout sweep: a
// processing task whose started_at predates the 0-second timeout is flipped to
// failed, exercising the tn>0 branch and the `if (timed_out) *timed_out` arm that
// the other RunCleanupNow tests (which pass timed_out=nullptr) never reach.
TEST(TaskCleanupCronRunTest, RunCleanupNowReportsTimedOutViaOutParam) {
    TaskManager mgr;
    ASSERT_TRUE(mgr.Init(":memory:").ok());
    auto p = mgr.CreateTask(MakeTask("dt"));
    ASSERT_TRUE(p.ok());
    mgr.MarkProcessing(p.value().task_id, 1);  // sets started_at ~now

    InMemoryGlobalConfig cfg;
    // Negative timeout pushes the cutoff into the future so started_at < cutoff is
    // unconditionally true (avoids same-second flakiness with timeout=0).
    cfg.Set("async.task_timeout_seconds", "-10");
    cfg.Set("async.zombie_task_threshold_hours", "9999");  // zombie window far out → no zombie
    cfg.Set("async.task_retention_days", "9999");          // nothing deleted
    TaskCleanupCron cron(&mgr, &cfg);

    int deleted = -1, zombies = -1, timed_out = -1;
    cron.RunCleanupNow(&deleted, &zombies, &timed_out);
    EXPECT_EQ(timed_out, 1);  // the processing row exceeded the 0s budget
    EXPECT_EQ(zombies, 0);
    EXPECT_EQ(deleted, 0);
    EXPECT_EQ(mgr.GetTask(p.value().task_id).value().status,
              std::string(task_status::kFailed));
    EXPECT_EQ(mgr.GetTask(p.value().task_id).value().error_code, "CX_ERR_TASK_TIMEOUT");
}

}  // namespace
}  // namespace cortrix::async
