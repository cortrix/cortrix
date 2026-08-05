// Tests for connector_routes.cpp (connector + browse endpoints) after the watcher
// fan-out refactor (TD-WATCHER-001). The routes now drive a DirWatcherRegistry
// (one OS watcher per directory, fan-out to N namespaces) instead of the MVP
// vector<WatcherEntry>. Uses the real in-process httplib server + NsPoolHarness,
// mirroring the test_flat_document_routes pattern. Namespace auto-create routes
// through a NiceMock<MockNSRouter> that accepts any CreateNamespace call.
//
// Coverage targets:
//   GET    /api/v1/connector/watchers                     — list (empty + populated + per-NS stats)
//   POST   /api/v1/connector/watchers                     — invalid JSON, missing path,
//                                                            dir-not-found, single-NS (compat),
//                                                            fan-out target_namespaces[]
//   DELETE /api/v1/connector/watchers/:id                 — not-found, success (+deleted_docs)
//   DELETE /api/v1/connector/watchers/:id/namespaces/:ns  — watcher single-NS unsubscribe,
//                                                            last-NS destroys watcher, not-found
//   POST   /api/v1/connector/watchers/:id/scan            — not-found, success
//   GET    /api/v1/connector/status                       — empty, populated
//   POST   /api/v1/connector/watch                        — invalid JSON, missing data_dir,
//                                                            dir-not-found, success (replace)
//   GET    /api/v1/connector/stats                        — empty, populated
//   POST   /api/v1/connector/scan                         — no watchers, success
//   GET    /api/v1/browse                                 — default path, real dir, non-dir 404
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/connector/dir_watcher_registry.h"
#include "cortrix/server/routes/connector_routes.h"
#include "cortrix/spc/spc_manager.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const std::string& value)
        : name_(name) {
        const char* previous = std::getenv(name_.c_str());
        if (previous != nullptr) {
            had_previous_ = true;
            previous_ = previous;
        }
        if (::setenv(name_.c_str(), value.c_str(), /*overwrite=*/1) != 0) {
            throw std::runtime_error("failed to set test environment variable: " + name_);
        }
    }

    ~ScopedEnvironmentVariable() {
        if (had_previous_) {
            ::setenv(name_.c_str(), previous_.c_str(), /*overwrite=*/1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

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

        // The importer canonicalizes data_dir (fs::canonical resolves the macOS
        // /var -> /private/var symlink temp_directory_path() returns un-resolved);
        // the /browse handler does the same via weakly_canonical. Compare API
        // responses against the canonical forms, matching the source.
        watch_dir_canon_ = fs::canonical(watch_dir_).string();
        temp_dir_canon_  = fs::canonical(temp_dir_).string();

        // The connector state needs a data_dir for watchers.json persistence.
        connector_state_.data_dir = (temp_dir_ / "state").string();
        fs::create_directories(connector_state_.data_dir);

        // Namespace pool.
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        // Accept any CreateNamespace call (namespace auto-create in Subscribe goes
        // through meta_mgr_, but the route signature still threads ns_router_).
        using ::testing::_;
        using ::testing::Return;
        ON_CALL(ns_router_, CreateNamespace(_))
            .WillByDefault(Return(Status::Ok()));

        spc_ = std::make_unique<StubSPC>();
        // No-auth CE deployment mode: auth disabled → WithAuth(auth, kPermAdmin, ...)
        // grants admin and passes through (the /browse directory picker is admin-gated
        // now, but stays usable without credentials in this mode). With auth ENABLED
        // (production), /browse requires an admin key — the security gate this models.
        auth_ = std::make_unique<ApiKeyAuth>();
        auth_->set_enabled(false);

        port_ = 18700 + (getpid() % 400);
        server_ = std::make_unique<httplib::Server>();
        // Watcher: RegisterConnectorRoutes builds the DirWatcherRegistry from the namespace pool
        // pool + the catalog router (ns_router_) + SPC; no NamespaceManager needed.
        RegisterConnectorRoutes(*server_, connector_state_,
                                harness_->ipool(), ns_router_,
                                *spc_, *auth_);

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

    // Add a watcher and return its id (single-NS compat form).
    std::string AddWatcher(httplib::Client& cli, const std::string& dir,
                           const std::string& ns) {
        json req = {{"data_dir", dir}, {"namespace_name", ns}};
        auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
        EXPECT_TRUE(res);
        EXPECT_EQ(res->status, 200) << (res ? res->body : "no response");
        return res ? json::parse(res->body)["id"].get<std::string>() : "";
    }

    fs::path temp_dir_;
    fs::path watch_dir_;
    std::string watch_dir_canon_;
    std::string temp_dir_canon_;
    ConnectorState connector_state_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router_;
    std::unique_ptr<StubSPC> spc_;
    std::unique_ptr<ApiKeyAuth> auth_;
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

TEST_F(ConnectorRoutesTest, AddWatcherMissingPath400) {
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

TEST_F(ConnectorRoutesTest, AddWatcherSingleNsCompatReturnsId) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"data_dir", watch_dir_.string()},
                {"namespace_name", "default"}};
    auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(res) << "no response";
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("id"));
    EXPECT_FALSE(body["id"].get<std::string>().empty());
    EXPECT_EQ(body["directory"], watch_dir_canon_);
    EXPECT_EQ(body["subscriber_count"], 1);
    ASSERT_EQ(body["target_namespaces"].size(), 1u);
    EXPECT_EQ(body["target_namespaces"][0], "default");
    EXPECT_TRUE(body.contains("message"));
}

// Watcher: POST with target_namespaces[] creates ONE fan-out watcher.
TEST_F(ConnectorRoutesTest, AddWatcherFanoutMultiNs) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"path", watch_dir_.string()},
                {"target_namespaces", {"team_a", "team_b", "archive"}},
                {"recursive", true}};
    auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["directory"], watch_dir_canon_);
    EXPECT_EQ(body["subscriber_count"], 3);
    EXPECT_EQ(body["target_namespaces"].size(), 3u);

    // Exactly ONE watcher appears in the list (fan-out, not 3 rows).
    auto list = cli.Get("/api/v1/connector/watchers");
    ASSERT_TRUE(list);
    auto lbody = json::parse(list->body);
    EXPECT_EQ(lbody["watchers"].size(), 1u);
    EXPECT_EQ(lbody["watchers"][0]["subscriber_count"], 3);
}

TEST_F(ConnectorRoutesTest, AddWatcherEmptyTargetNamespaces400) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"path", watch_dir_.string()},
                {"target_namespaces", json::array()}};
    auto res = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(ConnectorRoutesTest, AddWatcherAppearsInListWithStats) {
    httplib::Client cli("127.0.0.1", port_);
    AddWatcher(cli, watch_dir_.string(), "default");

    auto list = cli.Get("/api/v1/connector/watchers");
    ASSERT_TRUE(list);
    EXPECT_EQ(list->status, 200);
    auto body = json::parse(list->body);
    ASSERT_EQ(body["watchers"].size(), 1u);
    EXPECT_EQ(body["watchers"][0]["directory"], watch_dir_canon_);
    EXPECT_EQ(body["watchers"][0]["subscriber_count"], 1);
    // Per-NS stats block keyed by namespace name.
    ASSERT_TRUE(body["watchers"][0].contains("stats"));
    EXPECT_TRUE(body["watchers"][0]["stats"].contains("default"));
    EXPECT_TRUE(body["watchers"][0]["stats"]["default"].contains("total_files"));
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
    std::string id = AddWatcher(cli, watch_dir_.string(), "default");
    ASSERT_FALSE(id.empty());

    auto del = cli.Delete("/api/v1/connector/watchers/" + id);
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 200) << del->body;
    auto body = json::parse(del->body);
    EXPECT_EQ(body["message"], "Watcher removed");
    EXPECT_TRUE(body.contains("deleted_docs"));

    auto list = cli.Get("/api/v1/connector/watchers");
    ASSERT_TRUE(list);
    EXPECT_EQ(json::parse(list->body)["watchers"].size(), 0u);
}

// ---------------------------------------------------------------------------
// DELETE /api/v1/connector/watchers/:id/namespaces/:ns  (watcher single-NS)
// ---------------------------------------------------------------------------

TEST_F(ConnectorRoutesTest, UnsubscribeSingleNsKeepsWatcher) {
    httplib::Client cli("127.0.0.1", port_);
    json req = {{"path", watch_dir_.string()},
                {"target_namespaces", {"team_a", "team_b", "archive"}}};
    auto add = cli.Post("/api/v1/connector/watchers", req.dump(), "application/json");
    ASSERT_TRUE(add);
    ASSERT_EQ(add->status, 200) << add->body;
    std::string id = json::parse(add->body)["id"];

    // Unsubscribe team_b only -> 2 remain, watcher stays.
    auto del = cli.Delete("/api/v1/connector/watchers/" + id + "/namespaces/team_b");
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 200) << del->body;
    auto body = json::parse(del->body);
    EXPECT_EQ(body["status"], "ok");
    EXPECT_EQ(body["remaining_subscribers"], 2);
    EXPECT_EQ(body["watcher_removed"], false);

    auto list = cli.Get("/api/v1/connector/watchers");
    auto lbody = json::parse(list->body);
    ASSERT_EQ(lbody["watchers"].size(), 1u);
    EXPECT_EQ(lbody["watchers"][0]["subscriber_count"], 2);
}

TEST_F(ConnectorRoutesTest, UnsubscribeLastNsDestroysWatcher) {
    httplib::Client cli("127.0.0.1", port_);
    std::string id = AddWatcher(cli, watch_dir_.string(), "solo");
    ASSERT_FALSE(id.empty());

    auto del = cli.Delete("/api/v1/connector/watchers/" + id + "/namespaces/solo");
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 200) << del->body;
    auto body = json::parse(del->body);
    EXPECT_EQ(body["remaining_subscribers"], 0);
    EXPECT_EQ(body["watcher_removed"], true);

    auto list = cli.Get("/api/v1/connector/watchers");
    EXPECT_EQ(json::parse(list->body)["watchers"].size(), 0u);
}

TEST_F(ConnectorRoutesTest, UnsubscribeUnknownWatcher404) {
    httplib::Client cli("127.0.0.1", port_);
    auto del = cli.Delete("/api/v1/connector/watchers/deadbeef/namespaces/x");
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 404);
}

TEST_F(ConnectorRoutesTest, UnsubscribeUnknownNs404) {
    httplib::Client cli("127.0.0.1", port_);
    std::string id = AddWatcher(cli, watch_dir_.string(), "team_a");
    ASSERT_FALSE(id.empty());
    auto del = cli.Delete("/api/v1/connector/watchers/" + id + "/namespaces/ghost");
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 404);
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
    std::string id = AddWatcher(cli, watch_dir_.string(), "default");
    ASSERT_FALSE(id.empty());

    auto scan = cli.Post("/api/v1/connector/watchers/" + id + "/scan",
                         "", "application/json");
    ASSERT_TRUE(scan);
    EXPECT_EQ(scan->status, 200) << scan->body;
    EXPECT_EQ(json::parse(scan->body)["message"], "Scan completed");
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
    AddWatcher(cli, watch_dir_.string(), "ns1");

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

    // Add two watchers (two dirs) via /connector/watchers first.
    fs::path watch2 = temp_dir_ / "watch2";
    fs::create_directories(watch2);
    AddWatcher(cli, watch_dir_.string(), "nsA");
    AddWatcher(cli, watch2.string(), "nsB");

    // /connector/watch should replace ALL watchers with just the one.
    json watch_req = {{"data_dir", watch_dir_.string()}, {"namespace_name", "solo"}};
    auto res = cli.Post("/api/v1/connector/watch", watch_req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["watching"], true);
    EXPECT_EQ(body["namespace_name"], "solo");

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
    AddWatcher(cli, watch_dir_.string(), "default");

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
    AddWatcher(cli, watch_dir_.string(), "default");

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
    ScopedEnvironmentVariable home("HOME", temp_dir_.string());
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/browse");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["path"], temp_dir_canon_);
    EXPECT_TRUE(body.contains("entries"));
}

TEST_F(ConnectorRoutesTest, BrowseSpecificDirReturnsSubdirs) {
    fs::create_directories(temp_dir_ / "alpha");
    fs::create_directories(temp_dir_ / "beta");

    httplib::Client cli("127.0.0.1", port_);
    std::string url = "/api/v1/browse?path=" + temp_dir_.string();
    auto res = cli.Get(url);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["path"], temp_dir_canon_);
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
    for (size_t i = 1; i < entries.size(); ++i) {
        EXPECT_LE(entries[i - 1]["name"].get<std::string>(),
                  entries[i]["name"].get<std::string>());
    }
}

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
// ConnectorEnsureRegistry unit — idempotent + restores from watchers.json
// ---------------------------------------------------------------------------

TEST(ConnectorEnsureRegistryUnit, IdempotentAndRestoresState) {
    auto tmp = fs::temp_directory_path() / ("cregunit_" + std::to_string(::getpid()));
    fs::create_directories(tmp);
    auto watch = tmp / "wd";
    fs::create_directories(watch);
    auto state_dir = tmp / "state";
    fs::create_directories(state_dir);

    // Seed a v2 watchers.json so LoadState has something to restore.
    {
        nlohmann::json j;
        j["version"] = 2;
        j["watchers"] = nlohmann::json::array();
        nlohmann::json w;
        w["directory"] = fs::canonical(watch).string();
        w["target_namespaces"] = {"team_a", "team_b"};
        w["recursive"] = true;
        j["watchers"].push_back(w);
        std::ofstream(state_dir / "watchers.json") << j.dump(2);
    }

    cortrix::test::NsPoolHarness harness(tmp / "pool");
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router;
    using ::testing::_;
    using ::testing::Return;
    ON_CALL(ns_router, CreateNamespace(_)).WillByDefault(Return(Status::Ok()));
    StubSPC spc;

    ConnectorState state;
    state.data_dir = state_dir.string();

    ConnectorEnsureRegistry(state, harness.ipool(), ns_router, spc);
    ASSERT_NE(state.registry, nullptr);
    DirWatcherRegistry* first = state.registry.get();
    auto watches = state.registry->ListWatches();
    ASSERT_EQ(watches.size(), 1u);
    EXPECT_EQ(watches[0].subscriber_count, 2);

    // Second call is a no-op (same registry instance, not rebuilt).
    ConnectorEnsureRegistry(state, harness.ipool(), ns_router, spc);
    EXPECT_EQ(state.registry.get(), first);

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

}  // namespace
}  // namespace cortrix
