// Tests for connector_routes.cpp (connector + browse endpoints).
// Uses the real in-process httplib server + NsPoolHarness, mirroring the
// test_flat_document_routes pattern.  All dependencies that need a real
// namespace (ConnectorAddWatcher / watchers POST) route through a
// NiceMock<MockNSRouter> that accepts any CreateNamespace call.
//
// Coverage targets:
//   GET  /api/v1/connector/watchers      — list (empty + populated)
//   POST /api/v1/connector/watchers      — invalid JSON, missing data_dir,
//                                          dir-not-found, success
//   DELETE /api/v1/connector/watchers/:id — not-found, success
//   POST /api/v1/connector/watchers/:id/scan — not-found, success
//   GET  /api/v1/connector/status        — empty, populated
//   POST /api/v1/connector/watch         — invalid JSON, missing data_dir,
//                                          dir-not-found, success
//   GET  /api/v1/connector/stats         — empty, populated
//   POST /api/v1/connector/scan          — no watchers, success
//   GET  /api/v1/browse                  — default path, real dir, non-dir 404
//
//   ConnectorAddWatcher (unit) — autostart=false path (no Start())
//   ConnectorSaveWatchers      — smoke: no data_dir set (returns early)
//   MakeWatcherId              — deterministic / different (dir, ns) pairs differ
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/connector/directory_importer.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/routes/connector_routes.h"
#include "cortrix/spc/spc_manager.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Minimal SPCManager stub (no real processing).
class StubSPC : public SPCManager {
public:
    StubSPC() : SPCManager() {}
    Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
    int CancelBySourcePath(const std::string&) override { return 0; }
    void Start() override {}
    void Stop() override {}
    size_t QueueSize() const override { return 0; }
    SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class ConnectorRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() /
                    ("cortrix_connector_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_);

        // A real directory we can legitimately "watch" in tests.
        watch_dir_ = temp_dir_ / "watch";
        fs::create_directories(watch_dir_);

        // The importer canonicalizes data_dir in Start() (fs::canonical resolves
        // the macOS /var -> /private/var symlink that temp_directory_path() hands
        // back un-resolved); the /browse handler does the same via weakly_canonical.
        // Compare API responses against the canonical forms, matching the source.
        watch_dir_canon_ = fs::canonical(watch_dir_).string();
        temp_dir_canon_  = fs::canonical(temp_dir_).string();

        // The connector state needs a data_dir for watchers.json persistence.
        connector_state_.data_dir = (temp_dir_ / "state").string();
        fs::create_directories(connector_state_.data_dir);

        // F05 pool.
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        // Accept any CreateNamespace call (namespace auto-create in AddWatcher).
        using ::testing::_;
        using ::testing::Return;
        ON_CALL(ns_router_, CreateNamespace(_))
            .WillByDefault(Return(Status::Ok()));

        spc_ = std::make_unique<StubSPC>();
        auth_ = std::make_unique<ApiKeyAuth>();  // no keys loaded — routes are NoAuth

        // RegisterConnectorRoutes takes a NamespaceManager& (unused in route bodies,
        // but the parameter is a reference so we must provide a real object).
        fs::create_directories(temp_dir_ / "meta");
        meta_mgr_ = std::make_unique<NamespaceManager>(
            (temp_dir_ / "meta").string());
        meta_mgr_->Init();

        port_ = 18700 + (getpid() % 400);
        server_ = std::make_unique<httplib::Server>();
        RegisterConnectorRoutes(*server_, connector_state_,
                                harness_->ipool(), ns_router_,
                                *meta_mgr_, *spc_, *auth_);

        server_thread_ = std::thread([this]() {
            server_->listen("127.0.0.1", port_);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    fs::path temp_dir_;
    fs::path watch_dir_;
    std::string watch_dir_canon_;  // fs::canonical(watch_dir_) — see SetUp()
    std::string temp_dir_canon_;   // fs::canonical(temp_dir_)  — see SetUp()
    ConnectorState connector_state_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router_;
    std::unique_ptr<StubSPC> spc_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<NamespaceManager> meta_mgr_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_;
};

// ---------------------------------------------------------------------------
// GET /api/v1/connector/watchers — list
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, ListWatchersEmptyOnStart) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/connector/watchers");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("watchers"));
    EXPECT_EQ(body["watchers"].size(), 0u);
}

// ---------------------------------------------------------------------------
// POST /api/v1/connector/watchers — add watcher
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, AddWatcherInvalidJson400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/connector/watchers", "not-json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("error"));
}

TEST_F(ConnectorRoutesTest, AddWatcherMissingDataDir400) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"namespace_name", "default"}};
    auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(ConnectorRoutesTest, AddWatcherNonexistentDir404) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"data_dir", "/tmp/cortrix_nonexistent_dir_xyz987"},
                {"namespace_name", "default"}};
    auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

TEST_F(ConnectorRoutesTest, AddWatcherSuccessReturnsId) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"data_dir", watch_dir_.string()},
                {"namespace_name", "default"}};
    auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(res) << "no response";
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("id"));
    EXPECT_FALSE(body["id"].get<std::string>().empty());
    EXPECT_EQ(body["data_dir"], watch_dir_canon_);
    EXPECT_EQ(body["namespace_name"], "default");
    EXPECT_TRUE(body.contains("message"));
}

TEST_F(ConnectorRoutesTest, AddWatcherAppearsInList) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"data_dir", watch_dir_.string()}, {"namespace_name", "default"}};
    auto add = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(add);
    ASSERT_EQ(add->status, 200) << add->body;

    auto list = cli.Get("/api/v1/connector/watchers");
    ASSERT_TRUE(list);
    EXPECT_EQ(list->status, 200);
    auto body = json::parse(list->body);
    EXPECT_EQ(body["watchers"].size(), 1u);
    EXPECT_EQ(body["watchers"][0]["data_dir"], watch_dir_canon_);
}

// ---------------------------------------------------------------------------
// DELETE /api/v1/connector/watchers/:id
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, DeleteWatcherNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/connector/watchers/deadbeef");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

TEST_F(ConnectorRoutesTest, DeleteWatcherSuccess) {
    httplib::Client cli("127.0.0.1", port_);

    // Add a watcher first.
    json add_req = {{"data_dir", watch_dir_.string()}, {"namespace_name", "default"}};
    auto add = cli.Post("/api/v1/connector/watchers", add_req.dump(), "application/json");
    ASSERT_TRUE(add);
    ASSERT_EQ(add->status, 200) << add->body;
    std::string watcher_id = json::parse(add->body)["id"];

    // Delete it.
    auto del = cli.Delete("/api/v1/connector/watchers/" + watcher_id);
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 200) << del->body;
    auto body = json::parse(del->body);
    EXPECT_EQ(body["message"], "Watcher removed");
    EXPECT_TRUE(body.contains("deleted_docs"));

    // Verify it is gone from the list.
    auto list = cli.Get("/api/v1/connector/watchers");
    ASSERT_TRUE(list);
    auto list_body = json::parse(list->body);
    EXPECT_EQ(list_body["watchers"].size(), 0u);
}

// ---------------------------------------------------------------------------
// POST /api/v1/connector/watchers/:id/scan
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, ScanWatcherNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/connector/watchers/deadbeef/scan", "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(ConnectorRoutesTest, ScanWatcherSuccess) {
    httplib::Client cli("127.0.0.1", port_);

    // Add a watcher first.
    json add_req = {{"data_dir", watch_dir_.string()}, {"namespace_name", "default"}};
    auto add = cli.Post("/api/v1/connector/watchers", add_req.dump(), "application/json");
    ASSERT_TRUE(add);
    ASSERT_EQ(add->status, 200) << add->body;
    std::string watcher_id = json::parse(add->body)["id"];

    // Trigger scan.
    auto scan = cli.Post("/api/v1/connector/watchers/" + watcher_id + "/scan",
                         "", "application/json");
    ASSERT_TRUE(scan);
    EXPECT_EQ(scan->status, 200) << scan->body;
    auto body = json::parse(scan->body);
    EXPECT_EQ(body["message"], "Scan completed");
}

// ---------------------------------------------------------------------------
// GET /api/v1/connector/status — backward-compat single-watcher
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, StatusEmptyState) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/connector/status");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["enabled"], false);
    EXPECT_EQ(body["watching"], false);
    EXPECT_EQ(body["status"], "not_configured");
    EXPECT_EQ(body["watcher_count"], 0);
}

TEST_F(ConnectorRoutesTest, StatusWithWatcherReflectsFirstWatcher) {
    httplib::Client cli("127.0.0.1", port_);

    json add_req = {{"data_dir", watch_dir_.string()}, {"namespace_name", "ns1"}};
    auto add = cli.Post("/api/v1/connector/watchers", add_req.dump(), "application/json");
    ASSERT_TRUE(add);
    ASSERT_EQ(add->status, 200) << add->body;

    auto res = cli.Get("/api/v1/connector/status");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["enabled"], true);
    EXPECT_TRUE(body.contains("watch_dir"));
    EXPECT_EQ(body["namespace_name"], "ns1");
    EXPECT_EQ(body["watcher_count"], 1);
}

// ---------------------------------------------------------------------------
// POST /api/v1/connector/watch — backward-compat single-watcher replace
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, WatchCompatInvalidJson400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/connector/watch", "bad json!", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(ConnectorRoutesTest, WatchCompatMissingDataDir400) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"namespace_name", "default"}};
    auto res = cli.Post("/api/v1/connector/watch", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(ConnectorRoutesTest, WatchCompatDirNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"data_dir", "/no/such/path/xyz888"}};
    auto res = cli.Post("/api/v1/connector/watch", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(ConnectorRoutesTest, WatchCompatSuccessReplacesWatchers) {
    httplib::Client cli("127.0.0.1", port_);

    // Add two watchers via /connector/watchers first.
    fs::path watch2 = temp_dir_ / "watch2";
    fs::create_directories(watch2);
    json req1 = {{"data_dir", watch_dir_.string()}, {"namespace_name", "nsA"}};
    json req2 = {{"data_dir", watch2.string()}, {"namespace_name", "nsB"}};
    cli.Post("/api/v1/connector/watchers", req1.dump(), "application/json");
    cli.Post("/api/v1/connector/watchers", req2.dump(), "application/json");

    // /connector/watch should replace ALL watchers with just the one.
    json watch_req = {{"data_dir", watch_dir_.string()}, {"namespace_name", "solo"}};
    auto res = cli.Post("/api/v1/connector/watch", watch_req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["watching"], true);
    EXPECT_EQ(body["namespace_name"], "solo");

    // Only one watcher now.
    auto list = cli.Get("/api/v1/connector/watchers");
    ASSERT_TRUE(list);
    EXPECT_EQ(json::parse(list->body)["watchers"].size(), 1u);
}

// ---------------------------------------------------------------------------
// GET /api/v1/connector/stats — aggregate stats
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, StatsEmptyNoWatchers) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/connector/stats");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["enabled"], false);
    EXPECT_TRUE(body.contains("message"));
}

TEST_F(ConnectorRoutesTest, StatsWithWatcherContainsFields) {
    httplib::Client cli("127.0.0.1", port_);

    json add_req = {{"data_dir", watch_dir_.string()}, {"namespace_name", "default"}};
    auto add = cli.Post("/api/v1/connector/watchers", add_req.dump(), "application/json");
    ASSERT_TRUE(add);
    ASSERT_EQ(add->status, 200) << add->body;

    auto res = cli.Get("/api/v1/connector/stats");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["enabled"], true);
    EXPECT_TRUE(body.contains("watcher_count"));
    EXPECT_TRUE(body.contains("total_files"));
    EXPECT_TRUE(body.contains("imported"));
    EXPECT_TRUE(body.contains("elapsed_s"));
}

// ---------------------------------------------------------------------------
// POST /api/v1/connector/scan — scan all watchers
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, ScanAllNoWatchers400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/connector/scan", "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(ConnectorRoutesTest, ScanAllWithWatcherReturnsStats) {
    httplib::Client cli("127.0.0.1", port_);

    json add_req = {{"data_dir", watch_dir_.string()}, {"namespace_name", "default"}};
    auto add = cli.Post("/api/v1/connector/watchers", add_req.dump(), "application/json");
    ASSERT_TRUE(add);
    ASSERT_EQ(add->status, 200) << add->body;

    auto res = cli.Post("/api/v1/connector/scan", "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["message"], "Scan completed");
    EXPECT_TRUE(body.contains("total_files"));
    EXPECT_TRUE(body.contains("imported"));
}

// ---------------------------------------------------------------------------
// GET /api/v1/browse — directory picker
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, BrowseDefaultPathReturnsEntries) {
    httplib::Client cli("127.0.0.1", port_);
    // No "path" param — falls back to $HOME or "/".
    auto res = cli.Get("/api/v1/browse");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("path"));
    EXPECT_TRUE(body.contains("entries"));
}

TEST_F(ConnectorRoutesTest, BrowseSpecificDirReturnsSubdirs) {
    // Create a subdirectory so there is at least one entry.
    fs::create_directories(temp_dir_ / "alpha");
    fs::create_directories(temp_dir_ / "beta");

    httplib::Client cli("127.0.0.1", port_);
    std::string url = "/api/v1/browse?path=" + temp_dir_.string();
    auto res = cli.Get(url);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    // The handler returns weakly_canonical(path) (resolves /var -> /private/var).
    EXPECT_EQ(body["path"], temp_dir_canon_);
    // At least the two subdirs we created are in entries.
    EXPECT_GE(body["entries"].size(), 2u);
}

TEST_F(ConnectorRoutesTest, BrowseNonexistentPathReturns404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/browse?path=/no/such/path/xyz_never_exists");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

TEST_F(ConnectorRoutesTest, BrowseEntriesSortedAlphabetically) {
    fs::create_directories(temp_dir_ / "zzz");
    fs::create_directories(temp_dir_ / "aaa");
    fs::create_directories(temp_dir_ / "mmm");

    httplib::Client cli("127.0.0.1", port_);
    std::string url = "/api/v1/browse?path=" + temp_dir_.string();
    auto res = cli.Get(url);
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    auto& entries = body["entries"];
    // Verify sorted order.
    for (size_t i = 1; i < entries.size(); ++i) {
        EXPECT_LE(entries[i - 1]["name"].get<std::string>(),
                  entries[i]["name"].get<std::string>());
    }
}

// Hidden directories (name starts with '.') should not appear.
TEST_F(ConnectorRoutesTest, BrowseHiddenDirsExcluded) {
    fs::create_directories(temp_dir_ / ".hidden_dir");
    fs::create_directories(temp_dir_ / "visible_dir");

    httplib::Client cli("127.0.0.1", port_);
    std::string url = "/api/v1/browse?path=" + temp_dir_.string();
    auto res = cli.Get(url);
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    for (const auto& entry : body["entries"]) {
        EXPECT_FALSE(entry["name"].get<std::string>().empty());
        EXPECT_NE(entry["name"].get<std::string>()[0], '.');
    }
}

// ---------------------------------------------------------------------------
// ConnectorSaveWatchers unit — no data_dir → early return (no crash)
// ---------------------------------------------------------------------------

TEST(ConnectorSaveWatchersUnit, NoDataDirReturnsSilently) {
    ConnectorState state;
    // data_dir is empty: the function should return without touching the filesystem.
    EXPECT_NO_FATAL_FAILURE(ConnectorSaveWatchers(state));
}

// ---------------------------------------------------------------------------
// ConnectorAddWatcher unit — autostart=false (no Start() called, no real scan)
// ---------------------------------------------------------------------------

TEST(ConnectorAddWatcherUnit, AutostartFalseSucceeds) {
    // Stand up a minimal harness to get a pool + ns_router.
    auto tmp = fs::temp_directory_path() / ("cwunit_" + std::to_string(::getpid()));
    fs::create_directories(tmp);
    auto watch = tmp / "wd";
    fs::create_directories(watch);

    cortrix::test::NsPoolHarness harness(tmp / "pool");
    ASSERT_TRUE(harness.Admit("myns").ok());

    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router;
    using ::testing::_;
    using ::testing::Return;
    ON_CALL(ns_router, CreateNamespace(_)).WillByDefault(Return(Status::Ok()));

    StubSPC spc;
    ConnectorState state;
    state.data_dir = (tmp / "state").string();
    fs::create_directories(state.data_dir);

    WatcherEntry* entry = nullptr;
    Status s = ConnectorAddWatcher(state, ns_router, harness.ipool(), spc,
                                   watch.string(), "myns", &entry,
                                   /*autostart=*/false);
    EXPECT_TRUE(s.ok()) << s.message();
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->importer->GetConfig().data_dir, watch.string());
    EXPECT_EQ(entry->importer->GetConfig().namespace_name, "myns");
    EXPECT_EQ(state.watchers.size(), 1u);

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST(ConnectorAddWatcherUnit, SameDirNsReplacesPreviousWatcher) {
    auto tmp = fs::temp_directory_path() / ("cwunit2_" + std::to_string(::getpid()));
    fs::create_directories(tmp);
    auto watch = tmp / "wd";
    fs::create_directories(watch);

    cortrix::test::NsPoolHarness harness(tmp / "pool");
    ASSERT_TRUE(harness.Admit("ns").ok());

    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router;
    using ::testing::_;
    using ::testing::Return;
    ON_CALL(ns_router, CreateNamespace(_)).WillByDefault(Return(Status::Ok()));

    StubSPC spc;
    ConnectorState state;

    // Add once.
    ConnectorAddWatcher(state, ns_router, harness.ipool(), spc,
                        watch.string(), "ns", nullptr, false);
    EXPECT_EQ(state.watchers.size(), 1u);
    std::string first_id = state.watchers[0].id;

    // Add again with the same (dir, ns) — should replace, not duplicate.
    ConnectorAddWatcher(state, ns_router, harness.ipool(), spc,
                        watch.string(), "ns", nullptr, false);
    EXPECT_EQ(state.watchers.size(), 1u);
    EXPECT_EQ(state.watchers[0].id, first_id);  // same deterministic ID

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST(ConnectorAddWatcherUnit, DifferentNsProducesDifferentId) {
    auto tmp = fs::temp_directory_path() / ("cwunit3_" + std::to_string(::getpid()));
    fs::create_directories(tmp);
    auto watch = tmp / "wd";
    fs::create_directories(watch);

    cortrix::test::NsPoolHarness harness(tmp / "pool");
    ASSERT_TRUE(harness.Admit("nsA").ok());
    ASSERT_TRUE(harness.Admit("nsB").ok());

    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router;
    using ::testing::_;
    using ::testing::Return;
    ON_CALL(ns_router, CreateNamespace(_)).WillByDefault(Return(Status::Ok()));

    StubSPC spc;
    ConnectorState state;

    WatcherEntry* e1 = nullptr;
    WatcherEntry* e2 = nullptr;
    ConnectorAddWatcher(state, ns_router, harness.ipool(), spc,
                        watch.string(), "nsA", &e1, false);
    ConnectorAddWatcher(state, ns_router, harness.ipool(), spc,
                        watch.string(), "nsB", &e2, false);

    ASSERT_NE(e1, nullptr);
    ASSERT_NE(e2, nullptr);
    EXPECT_NE(e1->id, e2->id);  // different namespaces → different watcher IDs
    EXPECT_EQ(state.watchers.size(), 2u);

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

}  // namespace
}  // namespace cortrix
