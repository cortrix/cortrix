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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <httplib.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cortrix/async/task_info.h"
#include "cortrix/config/config.h"
#include "cortrix/logging/logging.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/server/bootstrap.h"

extern char** environ;

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
        // RunServer leaves its logger writing to this fixture's log file (its exit
        // path only flushes). Restore a default stdout logger before the file is
        // removed, so later tests in a monolithic run log somewhere real.
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

// ---------------------------------------------------------------------------
// #196: a start whose HTTP bind fails while document tasks are queued must end
// with RunServer's own return value. Its exit path used to drop spdlog's default
// logger while the task workers were still failing those tasks and logging, so the
// process died with SIGSEGV instead of returning 1. The statement runs in a
// re-executed child (threadsafe style). Exit code 3 means RunServer returned 1: a
// crash or a sanitizer report cannot produce it, but every path that returns 1 maps
// to it, so the stderr pattern pins the bind failure and shows that a worker logged
// after it. Neither check may be dropped.
// ---------------------------------------------------------------------------

std::filesystem::path BootstrapExitDir(pid_t parent) {
    return std::filesystem::path(::testing::TempDir()) /
           ("bootstrap_exit_" + std::to_string(parent));
}

// 20,000 queued document tasks whose input files do not exist, in one transaction.
// filepath_canonical is set so that TaskManager::Init's backfill skips the rows.
bool SeedQueuedTasks(const std::string& db_path, const std::string& missing_dir,
                     int count) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt* stmt = nullptr;
    ok = ok && sqlite3_prepare_v2(db,
                                  "INSERT INTO tasks (task_id, namespace_id, filename, "
                                  "filepath, filepath_canonical, created_at, updated_at) "
                                  "VALUES (?, 'default', ?, ?, ?, "
                                  "'2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z')",
                                  -1, &stmt, nullptr) == SQLITE_OK;
    for (int i = 0; ok && i < count; ++i) {
        const std::string id = "exit_task_" + std::to_string(i);
        const std::string name = id + ".txt";
        const std::string path = missing_dir + "/" + name;
        ok = sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_bind_text(stmt, 3, path.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_bind_text(stmt, 4, path.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_step(stmt) == SQLITE_DONE && sqlite3_reset(stmt) == SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    ok = ok && sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
    int queued = -1;
    sqlite3_stmt* count_stmt = nullptr;
    if (ok && sqlite3_prepare_v2(db, "SELECT count(*) FROM tasks WHERE status = 'queued'", -1,
                                 &count_stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(count_stmt) == SQLITE_ROW) {
        queued = sqlite3_column_int(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);
    sqlite3_close(db);
    return ok && queued == count;
}

[[noreturn]] void BindFailureWithQueuedTasks() {
    alarm(180);
    const pid_t parent = getppid();

    // No CORTRIX_ variable from the caller's shell may change the path to the bind.
    // The names are collected first: unsetting while walking environ skips entries.
    std::vector<std::string> names;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string entry(*e);
        if (entry.rfind("CORTRIX_", 0) == 0) names.push_back(entry.substr(0, entry.find('=')));
    }
    for (const std::string& name : names) unsetenv(name.c_str());
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        if (std::string(*e).rfind("CORTRIX_", 0) == 0) std::_Exit(5);
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = BootstrapExitDir(parent);
    fs::remove_all(base, ec);
    ec.clear();
    fs::create_directories(base / "data", ec);
    if (ec) std::_Exit(7);

    // Hold a loopback port without SO_REUSEADDR or SO_REUSEPORT, so the server's own
    // bind to it fails; the metrics server is pointed at it as well, which only warns.
    const int holder = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (holder < 0 || ::bind(holder, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(holder, 1) != 0 ||
        ::getsockname(holder, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        std::_Exit(8);
    }
    const int port = ntohs(addr.sin_port);
    setenv("CORTRIX_METRICS_PORT", std::to_string(port).c_str(), 1);

    const fs::path config = base / "config.yaml";
    {
        std::ofstream cfg(config);
        cfg << "server:\n"
            << "  host: \"127.0.0.1\"\n"
            << "  port: " << port << "\n"
            << "namespace:\n"
            << "  data_dir: " << (base / "data").string() << "\n"
            << "log:\n"
            << "  level: \"info\"\n"
            << "  format: \"text\"\n"
            << "  output: \"stderr\"\n";
        if (!cfg) std::_Exit(7);
    }

    const std::string tasks_db = (base / "data" / "tasks.db").string();
    {
        async::TaskManager schema;  // creates the tasks table, then closes
        if (!schema.Init(tasks_db).ok()) std::_Exit(6);
    }
    if (!SeedQueuedTasks(tasks_db, (base / "missing").string(), 20000)) std::_Exit(6);

    std::string program = "cortrix-server";
    std::string flag = "--config";
    std::string config_path = config.string();
    char* argv[] = {program.data(), flag.data(), config_path.data()};
    const int rc = server::RunServer(3, argv);
    fs::remove_all(base, ec);
    std::_Exit(rc == 1 ? 3 : 4);
}

TEST(BootstrapExitDeathTest, BindFailureWithQueuedTasksExitsCleanly) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(BindFailureWithQueuedTasks(), ::testing::ExitedWithCode(3),
                "Server failed: Failed to bind.*TaskFinalizer: task failed");
    std::error_code ec;
    std::filesystem::remove_all(BootstrapExitDir(getpid()), ec);
}

}  // namespace
}  // namespace cortrix
