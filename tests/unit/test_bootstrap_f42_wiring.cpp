// Regression coverage for issue #34: the F42 cleanup cron and the §6.1 startup
// requeue must run on the STARTED SERVER path, not merely exist as testable
// classes. Every prior F42 test constructed TaskCleanupCron / called
// RequeueStaleProcessing directly, so removing the production construction or
// the bootstrap call left the whole suite green — the exact failure mode #34
// reports. This test boots the real RunServer() entry (stub model mode) on a
// private data dir, observes the live effects, then SIGTERMs it and requires a
// clean exit.
//
// Live signals asserted:
//   1. A fresh `processing` row seeded before boot leaves its seeded state —
//      only the bootstrap requeue can make it eligible again (workers dequeue
//      `queued` rows only, and the cron's scheduled sweep waits for 02:00 UTC).
//   2. A beyond-threshold `processing` row is left untouched — pins the §6.1
//      boundary (requeue only rows younger than the zombie threshold) on the
//      live path, guarding against a requeue-everything implementation.
//   3. The startup log carries the cron-start line and RunServer returns 0
//      after SIGTERM, so construction, Start() and the Stop()/join ordering
//      all happened on the production path.

#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <unistd.h>

#include "cortrix/async/task_info.h"
#include "cortrix/config/config.h"
#include "cortrix/logging/logging.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/server/bootstrap.h"

namespace cortrix {
namespace {

std::string Slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class BootstrapF42WiringTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = "/tmp/cortrix_bootstrap_f42_" + std::to_string(getpid());
        data_dir_ = base_ + "/data";
        log_file_ = base_ + "/server.log";
        config_path_ = base_ + "/config.yaml";
        port_ = 18000 + (getpid() % 10000);
        std::system(("rm -rf " + base_ + " && mkdir -p " + data_dir_).c_str());

        std::ofstream cfg(config_path_);
        cfg << "server:\n"
            << "  host: \"127.0.0.1\"\n"
            << "  port: " << port_ << "\n"
            << "namespace:\n"
            << "  data_dir: " << data_dir_ << "\n"
            << "log:\n"
            << "  level: \"info\"\n"
            << "  format: \"text\"\n"
            << "  output: " << log_file_ << "\n";
        cfg.close();
        // Empty model paths (the struct defaults) select stub mode; the env var
        // additionally keeps any model preflight from failing the boot.
        setenv("CORTRIX_ALLOW_MISSING_MODELS", "1", 1);
    }
    void TearDown() override {
        // RunServer's exit path calls ShutdownLogging(), dropping the global
        // spdlog state. Restore a default stdout logger so later tests in a
        // monolithic (single-process) run keep a live logging backend — without
        // this, the next CORTRIX_LOG after this fixture crashes the binary.
        LogConfig lc;
        InitLogging(lc);
        std::system(("rm -rf " + base_).c_str());
    }

    std::string base_, data_dir_, log_file_, config_path_;
    int port_ = 0;
};

TEST_F(BootstrapF42WiringTest, StartedServerRunsRequeueAndStartsCron) {
    constexpr int kSeedWorker = 424242;  // sentinel: no real worker uses this id

    // Seed tasks.db BEFORE boot: one fresh processing row (must be re-queued)
    // and one 25h-stale processing row (must be left for the zombie sweep).
    std::string fresh_id, stale_id;
    {
        async::TaskManager mgr;
        ASSERT_TRUE(mgr.Init(data_dir_ + "/tasks.db").ok());
        async::TaskInfo t;
        t.namespace_id = "default";
        t.filename = "does_not_exist.txt";
        t.filepath = base_ + "/does_not_exist.txt";
        t.content_hash = "h-fresh";
        auto fresh = mgr.CreateTask(t);
        ASSERT_TRUE(fresh.ok());
        fresh_id = fresh.value().task_id;
        ASSERT_TRUE(mgr.MarkProcessing(fresh_id, kSeedWorker).ok());

        t.content_hash = "h-stale";
        auto stale = mgr.CreateTask(t);
        ASSERT_TRUE(stale.ok());
        stale_id = stale.value().task_id;
        ASSERT_TRUE(mgr.MarkProcessing(stale_id, kSeedWorker).ok());
    }
    {
        // Push the stale row's updated_at beyond the 24h default threshold.
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open((data_dir_ + "/tasks.db").c_str(), &db), SQLITE_OK);
        const std::string sql =
            "UPDATE tasks SET updated_at='2020-01-01T00:00:00Z' WHERE task_id='" +
            stale_id + "'";
        ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }

    // Boot the real server entry in-process.
    std::string program = "cortrix-server";
    std::string flag = "--config";
    char* argv[] = {program.data(), flag.data(), config_path_.data()};
    int rc = -1;
    std::thread server([&] { rc = server::RunServer(3, argv); });

    // Wait until the HTTP server answers (any response = listening; bootstrap —
    // including the requeue and cron start — has completed by then).
    bool up = false;
    for (int i = 0; i < 300 && !up; ++i) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(0, 200000);
        if (cli.Get("/health")) up = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(up) << "server did not start listening on port " << port_;

    ::raise(SIGTERM);
    server.join();
    EXPECT_EQ(rc, 0) << "graceful shutdown did not return 0";

    // Signal 3: the production path constructed and started the cron, and the
    // requeue ran (count line only appears when rows were re-queued).
    const std::string log = Slurp(log_file_);
    EXPECT_NE(log.find("F42 task cleanup cron started"), std::string::npos)
        << "cron start line missing from startup log";
    EXPECT_NE(log.find("re-queued"), std::string::npos)
        << "crash-recovery requeue line missing from startup log";

    // Signals 1 + 2, read back after shutdown (no lock contention).
    async::TaskManager check;
    ASSERT_TRUE(check.Init(data_dir_ + "/tasks.db").ok());
    auto fresh = check.GetTask(fresh_id);
    ASSERT_TRUE(fresh.ok());
    // Re-queued rows become `queued`; a worker may then legitimately dequeue and
    // fail them (the seeded file does not exist). Every reachable state differs
    // from the seeded (processing, sentinel-worker) pair — which is exactly the
    // state the row would still be in if the bootstrap requeue were removed.
    EXPECT_FALSE(fresh.value().status == std::string(async::task_status::kProcessing) &&
                 fresh.value().worker_id == kSeedWorker)
        << "fresh processing row still in seeded state: bootstrap requeue did not run "
           "(status=" << fresh.value().status << ")";

    auto stale = check.GetTask(stale_id);
    ASSERT_TRUE(stale.ok());
    EXPECT_EQ(stale.value().status, std::string(async::task_status::kProcessing))
        << "beyond-threshold row must be left for the zombie sweep, not re-queued";
    EXPECT_EQ(stale.value().worker_id, kSeedWorker);
}

// The /ready endpoint on the STARTED server executes the real probes registered
// in bootstrap.cpp (catalog, spc_pipeline, disk, secret_provider) — replacing
// the deleted test_readiness.cpp tests that asserted test-local lambda
// "mirrors" of these probes without ever running the real ones. Asserts the
// live component details the mirrors used to fake: catalog_db_open=true on the
// open handle, workers>0 + queue_depth after spc_mgr.Start().
TEST_F(BootstrapF42WiringTest, StartedServerServesRealReadinessProbes) {
    std::string program = "cortrix-server";
    std::string flag = "--config";
    char* argv[] = {program.data(), flag.data(), config_path_.data()};
    int rc = -1;
    std::thread server([&] { rc = server::RunServer(3, argv); });

    httplib::Response ready_res;
    bool got = false;
    for (int i = 0; i < 300 && !got; ++i) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(0, 200000);
        if (auto r = cli.Get("/api/v1/system/health/ready")) {
            ready_res = *r;
            got = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    ASSERT_TRUE(got) << "server did not answer /ready on port " << port_;

    ::raise(SIGTERM);
    server.join();
    EXPECT_EQ(rc, 0);

    nlohmann::json body = nlohmann::json::parse(ready_res.body,
                                                /*cb=*/nullptr,
                                                /*allow_exceptions=*/false);
    ASSERT_FALSE(body.is_discarded()) << ready_res.body;
    ASSERT_TRUE(body.contains("components")) << ready_res.body;
    const auto& comps = body["components"];

    // catalog: the REAL probe reads the open catalog.db handle.
    ASSERT_TRUE(comps.contains("catalog")) << ready_res.body;
    EXPECT_EQ(comps["catalog"]["status"], "ok") << ready_res.body;
    EXPECT_EQ(comps["catalog"]["catalog_db_open"], true) << ready_res.body;

    // spc_pipeline: the REAL probe reads the started WorkerPool + SPC queue.
    ASSERT_TRUE(comps.contains("spc_pipeline")) << ready_res.body;
    EXPECT_EQ(comps["spc_pipeline"]["status"], "ok") << ready_res.body;
    EXPECT_GT(comps["spc_pipeline"]["workers"].get<int>(), 0) << ready_res.body;
    EXPECT_TRUE(comps["spc_pipeline"].contains("queue_depth")) << ready_res.body;
}

}  // namespace
}  // namespace cortrix
