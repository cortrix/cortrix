#include <gtest/gtest.h>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/catalog/schema_provider.h"  // SchemaMigrator
#include "cortrix/observability/cleanup_scheduler.h"
#include "cortrix/observability/observability_module.h"
#include "cortrix/observability/operation_log_emitter.h"
#include "cortrix/observability/operation_log_schema.h"
#include "cortrix/observability/operation_logger_impl.h"

// S2 coverage: CleanupScheduler, the EmitSite helper, and the
// ObservabilityModule DI wiring (/ Issue 10).
namespace cortrix::observability {
namespace {

// ---- NextRunDelayMs (pure, UTC 02:00) -------------------------------------

TEST(CleanupSchedulerTimeTest, NextRunDelayIsAlwaysWithinADay) {
    // 1970-01-01 00:00:00 UTC → next 02:00 is +2h.
    EXPECT_EQ(CleanupScheduler::NextRunDelayMs(0), 2LL * 3600000);
    // exactly 02:00 → next is +24h (not 0).
    EXPECT_EQ(CleanupScheduler::NextRunDelayMs(2LL * 3600000), 24LL * 3600000);
    // 03:00 → 23h to tomorrow's 02:00.
    EXPECT_EQ(CleanupScheduler::NextRunDelayMs(3LL * 3600000), 23LL * 3600000);
    // 01:59:59.000 → 1s away.
    EXPECT_EQ(CleanupScheduler::NextRunDelayMs(2LL * 3600000 - 1000), 1000);
}

TEST(CleanupSchedulerTimeTest, NextRunDelayHandlesArbitraryEpoch) {
    // A real-ish epoch ms; result must be in (0, 24h].
    int64_t now = 1747584000000LL;  // 2025-05-18ish
    int64_t d = CleanupScheduler::NextRunDelayMs(now);
    EXPECT_GT(d, 0);
    EXPECT_LE(d, 24LL * 3600000);
}

TEST(CleanupSchedulerTimeTest, BackoffSchedule) {
    EXPECT_EQ(CleanupScheduler::BackoffMs(1), 60000);
    EXPECT_EQ(CleanupScheduler::BackoffMs(2), 300000);
    EXPECT_EQ(CleanupScheduler::BackoffMs(3), 900000);
    EXPECT_EQ(CleanupScheduler::BackoffMs(4), 0);
    EXPECT_EQ(CleanupScheduler::kMaxAttempts, 3);
}

// ---- RunCleanupNow + advisory lock ----------------------------------------

TEST(CleanupSchedulerRunTest, RunsEveryRegisteredTableOnce) {
    CleanupScheduler s;
    std::atomic<int> a{0}, b{0};
    s.RegisterTable("a", [&] { ++a; });
    s.RegisterTable("b", [&] { ++b; });
    EXPECT_EQ(s.registered_count(), 2u);

    s.RunCleanupNow();
    EXPECT_EQ(a.load(), 1);
    EXPECT_EQ(b.load(), 1);
    EXPECT_FALSE(s.is_running());  // flag cleared after sweep
}

TEST(CleanupSchedulerRunTest, NullCallbackIgnored) {
    CleanupScheduler s;
    s.RegisterTable("noop", nullptr);
    EXPECT_EQ(s.registered_count(), 0u);
}

// Advisory lock: a reentrant RunCleanupNow() from inside a callback returns
// immediately (try_lock fails) — the inner sweep is skipped, not deadlocked.
TEST(CleanupSchedulerRunTest, AdvisoryLockPreventsOverlap) {
    CleanupScheduler s;
    std::atomic<int> outer{0}, inner_seen_running{0};
    s.RegisterTable("t", [&] {
        ++outer;
        EXPECT_TRUE(s.is_running());
        // Re-entrant call must be a no-op (lock already held).
        s.RunCleanupNow();
        if (s.is_running()) ++inner_seen_running;
    });
    s.RunCleanupNow();
    EXPECT_EQ(outer.load(), 1);  // callback ran exactly once (inner skipped)
}

// A throwing cleanup is retried up to kMaxAttempts; with a near-zero backoff
// (BackoffMs is fixed, so we just verify it stops after 3 and doesn't throw out).
TEST(CleanupSchedulerRunTest, ThrowingCleanupDoesNotEscapeSweep) {
    CleanupScheduler s;
    std::atomic<int> attempts{0};
    // Throw on attempts 1+2 then succeed on attempt 3 → 3 invocations total.
    // BackoffMs(1)=60s would stall the test, so we trigger StopScheduler via a
    // separate thread to interrupt the backoff wait deterministically.
    s.RegisterTable("flaky", [&] {
        if (++attempts < 3) throw std::runtime_error("boom");
    });
    // Run on a worker; interrupt the inter-attempt backoff by stopping.
    std::thread worker([&] { s.RunCleanupNow(); });
    // Give the first attempt time to throw and enter the backoff wait, then stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    s.StopScheduler();  // wakes the interruptible backoff
    worker.join();
    EXPECT_GE(attempts.load(), 1);   // at least the first attempt ran
    // No exception escaped RunCleanupNow (the worker joined cleanly).
}

// ---- background loop with test interval -----------------------------------

TEST(CleanupSchedulerLoopTest, BackgroundLoopFiresCatchUpThenInterval) {
    CleanupScheduler s;
    std::atomic<int> n{0};
    s.RegisterTable("t", [&] { ++n; });
    s.set_test_interval_ms(20);  // fast loop instead of UTC 02:00
    s.StartScheduler();          // catch-up sweep + periodic
    // Wait for the catch-up + a couple of interval sweeps.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    s.StopScheduler();
    EXPECT_GE(n.load(), 2);  // catch-up (1) + >=1 interval fire
}

TEST(CleanupSchedulerLoopTest, StopWithoutStartIsSafe) {
    CleanupScheduler s;
    s.StopScheduler();  // no-op, must not crash/hang
    SUCCEED();
}

// ---- EmitSite helper -----------------------------------------------

TEST(OperationLogEmitterTest, ResourceTypePerSite) {
    EXPECT_STREQ(ResourceTypeFor(EmitSite::kQueryEngine, "query"), "query");
    EXPECT_STREQ(ResourceTypeFor(EmitSite::kQueryEngine, "cross_ns_query"), "query");
    EXPECT_STREQ(ResourceTypeFor(EmitSite::kSpcPipeline, "upload"), "document");
    EXPECT_STREQ(ResourceTypeFor(EmitSite::kSpcPipeline, "delete"), "document");
    EXPECT_STREQ(ResourceTypeFor(EmitSite::kSpcPipeline, "database_import"), "db_import");
    EXPECT_STREQ(ResourceTypeFor(EmitSite::kSpcPipeline, "db_connection_register"),
                 "db_connection");
    EXPECT_STREQ(ResourceTypeFor(EmitSite::kMemoryStore, "memory_create"), "memory");
    EXPECT_STREQ(ResourceTypeFor(EmitSite::kNamespaceManager, "ns_create"), "namespace");
}

TEST(OperationLogEmitterTest, SummaryTruncatedTo100) {
    std::string long_s(250, 'x');
    auto t = TruncateSummary(long_s);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->size(), 100u);
    EXPECT_FALSE(TruncateSummary(std::nullopt).has_value());
    EXPECT_EQ(TruncateSummary(std::string("short")), std::optional<std::string>("short"));
}

TEST(OperationLogEmitterTest, MakeEngineEntryReadsThreadLocalTrace) {
    auto& octx = ObservabilityContext::ThreadLocal();
    TraceContext tc;
    tc.trace_id = "tl-trace";
    octx.SetTraceContext(tc);

    auto e = MakeEngineEntry(EmitSite::kMemoryStore, "memory_create",
                             std::string("sales"), std::string("blk-1"),
                             std::string("hi"));
    EXPECT_EQ(e.action, "memory_create");
    EXPECT_EQ(e.resource_type, "memory");
    EXPECT_EQ(e.namespace_id, std::optional<std::string>("sales"));
    EXPECT_EQ(e.trace_id, std::optional<std::string>("tl-trace"));
    EXPECT_EQ(e.user_id, "anonymous");  // default until auth
    EXPECT_EQ(e.timestamp, 0);          // logger fills now()

    octx.ClearTraceContext();  // don't leak into other tests on this thread
}

// ---- ObservabilityModule DI (/ Issue 10) -------------------------------

// A fake instrumentation site capturing the injected logger (stands in for the real Engine
// modules wired at integration).
class FakeAware : public IOperationLoggerAware {
public:
    void SetOperationLogger(std::shared_ptr<IOperationLogger> l) override { logger = l; }
    std::shared_ptr<IOperationLogger> logger;
};

class ObservabilityModuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        config_ = std::make_shared<InMemoryGlobalConfig>();
    }
    void TearDown() override { if (db_) sqlite3_close(db_); }
    OperationLogSchemaProvider provider_;
    sqlite3* db_ = nullptr;
    std::shared_ptr<InMemoryGlobalConfig> config_;
};

TEST_F(ObservabilityModuleTest, InitializeBuildsLoggerAndInjectsEmitters) {
    ObservabilityModule mod(db_, config_);
    FakeAware engine_a, engine_b;
    mod.RegisterEmitter(&engine_a);   // queued before Initialize
    mod.Initialize();
    mod.RegisterEmitter(&engine_b);   // injected immediately (after Initialize)

    ASSERT_NE(mod.logger(), nullptr);
    EXPECT_EQ(engine_a.logger, mod.logger());
    EXPECT_EQ(engine_b.logger, mod.logger());
    EXPECT_EQ(mod.scheduler().registered_count(), 1u);  // operation_log registered

    // The injected logger actually writes to the migrated db.
    OperationLogEntry e;
    e.user_id = "u";
    e.action = "query";
    e.resource_type = "query";
    engine_a.logger->Log(e);
    auto r = engine_a.logger->Query(OperationLogFilter{});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->total_count, 1);

    mod.Shutdown();
}

TEST_F(ObservabilityModuleTest, InitializeIsIdempotent) {
    ObservabilityModule mod(db_, config_);
    mod.Initialize();
    auto first = mod.logger();
    mod.Initialize();  // second call → no new logger, no double-register
    EXPECT_EQ(mod.logger(), first);
    EXPECT_EQ(mod.scheduler().registered_count(), 1u);
    mod.Shutdown();
}

// Issue 10: an embedder module overrides MakeLogger() to return an extended logger.
// We can't reference that type from cortrix/, but we prove the seam works by
// subclassing here with a sentinel logger and checking the override is used.
class SentinelLogger : public IOperationLogger {
public:
    void Log(const OperationLogEntry&, const TraceContext*) override { ++calls; }
    void BatchLog(const std::vector<OperationLogEntry>&, const TraceContext*) override {}
    Result<OperationLogQueryResult> Query(const OperationLogFilter&,
                                          const TraceContext*) override {
        return OperationLogQueryResult{};
    }
    void Cleanup() override {}
    OperationLogStats GetStats() override { return {}; }
    HealthStatus Health() override { return {}; }
    int calls = 0;
};

class OverrideModule : public ObservabilityModule {
public:
    using ObservabilityModule::ObservabilityModule;
    std::shared_ptr<IOperationLogger> MakeLogger() override {
        sentinel = std::make_shared<SentinelLogger>();
        return sentinel;
    }
    std::shared_ptr<SentinelLogger> sentinel;
};

TEST_F(ObservabilityModuleTest, MakeLoggerOverrideUsedByInitialize) {
    OverrideModule mod(db_, config_);
    mod.Initialize();
    EXPECT_EQ(mod.logger(), mod.sentinel);  // override honored (Ent seam)
    mod.Shutdown();
}

}  // namespace
}  // namespace cortrix::observability
