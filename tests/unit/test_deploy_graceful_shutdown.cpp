#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cortrix/deploy/deploy_metrics.h"
#include "cortrix/deploy/graceful_shutdown.h"

// Deployment coverage: graceful shutdown — pending-task
// serialization round-trip, the ordered Run() phases with injected hooks, the
// drain-timeout → forced + persist path, the shutdown_status gauge feed, and the
// startup resume path (parse + resubmit + delete .pending_tasks.json).
namespace cortrix::deploy {
namespace {

PendingTask MakeTask(const std::string& doc_id) {
    PendingTask t;
    t.namespace_id = "ns1";
    t.filename = doc_id + ".pdf";
    t.filepath = "/tmp/" + doc_id;
    t.doc_id = doc_id;
    t.content_hash = "hash_" + doc_id;
    t.total_pages = 7;
    t.task_type = 1;
    return t;
}

// A unique temp data_dir per test so the .pending_tasks.json files don't collide.
std::string MakeTempDir(const std::string& tag) {
    std::string dir = "/tmp/deploy_gs_" + tag + "_" + std::to_string(::getpid());
    std::string cmd = "mkdir -p '" + dir + "'";
    (void)std::system(cmd.c_str());
    return dir;
}

class GracefulShutdownTest : public ::testing::Test {
protected:
    void SetUp() override { DeployMetrics::Instance().ResetForTest(); }
    void TearDown() override { DeployMetrics::Instance().ResetForTest(); }
};

TEST_F(GracefulShutdownTest, PendingSerializationRoundTrips) {
    std::vector<PendingTask> in = {MakeTask("a"), MakeTask("b")};
    nlohmann::json j = SerializePending(in);
    EXPECT_EQ(j["version"], 1);
    ASSERT_EQ(j["tasks"].size(), 2u);

    std::vector<PendingTask> out = DeserializePending(j);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].doc_id, "a");
    EXPECT_EQ(out[1].content_hash, "hash_b");
    EXPECT_EQ(out[0].total_pages, 7);
    EXPECT_EQ(out[1].task_type, 1);
}

TEST_F(GracefulShutdownTest, SubmitRequestConversionPreservesFields) {
    PendingTask t = MakeTask("x");
    async::SubmitRequest req = ToSubmitRequest(t);
    EXPECT_EQ(req.namespace_id, "ns1");
    EXPECT_EQ(req.doc_id, "x");
    EXPECT_EQ(req.content_hash, "hash_x");
    EXPECT_EQ(req.total_pages, 7);
    PendingTask back = FromSubmitRequest(req);
    EXPECT_EQ(back.filepath, t.filepath);
}

TEST_F(GracefulShutdownTest, CleanDrainRunsAllPhasesInOrderNoPersist) {
    ShutdownConfig cfg;
    cfg.grace_period_sec = 30;
    cfg.data_dir = MakeTempDir("clean");

    std::vector<std::string> order;
    bool persisted = false;
    GracefulShutdown::Hooks hooks;
    hooks.close_http     = [&] { order.push_back("close_http"); };
    hooks.drain_tasks    = [&](std::chrono::steady_clock::time_point) {
        order.push_back("drain");
        return std::vector<PendingTask>{};  // drained clean
    };
    hooks.flush_wal      = [&] { order.push_back("flush_wal"); };
    hooks.persist_memory = [&] { order.push_back("persist_memory"); };
    hooks.persist_file   = [&](const std::string&, const std::string&) {
        persisted = true; return true;
    };

    GracefulShutdown gs(cfg, hooks);
    ShutdownStatus st = gs.Run();

    // ordering: close_http → drain → flush_wal → persist_memory.
    std::vector<std::string> want = {"close_http", "drain", "flush_wal", "persist_memory"};
    EXPECT_EQ(order, want);
    EXPECT_FALSE(persisted);  // clean drain → no pending file written
    EXPECT_EQ(st, ShutdownStatus::kShuttingDown);  // not forced
    // gauge ended at 1 (shutting_down), never 2.
    EXPECT_EQ(DeployMetrics::Instance().ShutdownStatus(), 1);
}

TEST_F(GracefulShutdownTest, DrainTimeoutForcesAndPersists) {
    ShutdownConfig cfg;
    cfg.grace_period_sec = 30;
    cfg.data_dir = MakeTempDir("forced");

    std::string captured_path, captured_body;
    GracefulShutdown::Hooks hooks;
    hooks.drain_tasks = [&](std::chrono::steady_clock::time_point) {
        return std::vector<PendingTask>{MakeTask("leftover1"), MakeTask("leftover2")};
    };
    hooks.persist_file = [&](const std::string& p, const std::string& b) {
        captured_path = p; captured_body = b; return true;
    };

    GracefulShutdown gs(cfg, hooks);
    ShutdownStatus st = gs.Run();

    EXPECT_EQ(st, ShutdownStatus::kForced);
    EXPECT_EQ(DeployMetrics::Instance().ShutdownStatus(), 2);  // forced
    EXPECT_EQ(captured_path, PendingTasksPath(cfg.data_dir));
    // body is the serialized pending list.
    auto j = nlohmann::json::parse(captured_body);
    EXPECT_EQ(j["tasks"].size(), 2u);
    EXPECT_EQ(j["tasks"][0]["doc_id"], "leftover1");
}

TEST_F(GracefulShutdownTest, RunIsIdempotent) {
    ShutdownConfig cfg;
    cfg.data_dir = MakeTempDir("idem");
    int drains = 0;
    GracefulShutdown::Hooks hooks;
    hooks.drain_tasks = [&](std::chrono::steady_clock::time_point) {
        ++drains; return std::vector<PendingTask>{};
    };
    GracefulShutdown gs(cfg, hooks);
    gs.Run();
    gs.Run();  // second call is a no-op
    EXPECT_EQ(drains, 1);
}

TEST_F(GracefulShutdownTest, DeadlineHonorsGraceMinusReservedBudget) {
    // With grace=10 the drain deadline must be ~ now + (10 - 7) = 3s in the future.
    ShutdownConfig cfg;
    cfg.grace_period_sec = 10;
    cfg.data_dir = MakeTempDir("deadline");
    std::chrono::steady_clock::time_point seen;
    bool got = false;
    GracefulShutdown::Hooks hooks;
    hooks.drain_tasks = [&](std::chrono::steady_clock::time_point dl) {
        seen = dl; got = true; return std::vector<PendingTask>{};
    };
    auto before = std::chrono::steady_clock::now();
    GracefulShutdown gs(cfg, hooks);
    gs.Run();
    ASSERT_TRUE(got);
    auto budget = std::chrono::duration_cast<std::chrono::seconds>(seen - before).count();
    // 3s nominal; allow a small scheduling slop.
    EXPECT_GE(budget, 2);
    EXPECT_LE(budget, 4);
}

TEST_F(GracefulShutdownTest, ResumeOnStartupReadsAndDeletesFile) {
    ShutdownConfig cfg;
    cfg.data_dir = MakeTempDir("resume");

    // Write a real pending file via the production atomic writer.
    std::vector<PendingTask> tasks = {MakeTask("r1"), MakeTask("r2"), MakeTask("r3")};
    std::string path = PendingTasksPath(cfg.data_dir);
    ASSERT_TRUE(AtomicWriteFile(path, SerializePending(tasks).dump()));

    GracefulShutdown gs(cfg);
    std::vector<std::string> resumed_ids;
    int n = gs.ResumeOnStartup([&](const async::SubmitRequest& r) {
        resumed_ids.push_back(r.doc_id);
    });

    EXPECT_EQ(n, 3);
    ASSERT_EQ(resumed_ids.size(), 3u);
    EXPECT_EQ(resumed_ids[0], "r1");
    // file is consumed (deleted) after a successful resume.
    std::ifstream check(path);
    EXPECT_FALSE(check.good());
}

TEST_F(GracefulShutdownTest, ResumeOnStartupNoFileReturnsZero) {
    ShutdownConfig cfg;
    cfg.data_dir = MakeTempDir("nofile");
    GracefulShutdown gs(cfg);
    int n = gs.ResumeOnStartup([](const async::SubmitRequest&) {});
    EXPECT_EQ(n, 0);
}

TEST_F(GracefulShutdownTest, ResumeOnStartupCorruptFileLeavesItAndReturnsZero) {
    ShutdownConfig cfg;
    cfg.data_dir = MakeTempDir("corrupt");
    std::string path = PendingTasksPath(cfg.data_dir);
    ASSERT_TRUE(AtomicWriteFile(path, "{ this is not valid json "));

    GracefulShutdown gs(cfg);
    int n = gs.ResumeOnStartup([](const async::SubmitRequest&) {});
    EXPECT_EQ(n, 0);
    // corrupt file is left in place for operator inspection.
    std::ifstream check(path);
    EXPECT_TRUE(check.good());
    std::remove(path.c_str());  // cleanup
}

TEST_F(GracefulShutdownTest, AtomicWriteFileWritesContents) {
    std::string dir = MakeTempDir("atomic");
    std::string path = dir + "/out.json";
    ASSERT_TRUE(AtomicWriteFile(path, "hello-deploy"));
    std::ifstream in(path);
    std::ostringstream ss; ss << in.rdbuf();
    EXPECT_EQ(ss.str(), "hello-deploy");
    std::remove(path.c_str());
}

}  // namespace
}  // namespace cortrix::deploy
