// Targeted coverage for the residual uncovered arms of
// src/server/routes/connector_routes.cpp that test_connector_routes.cpp /
// test_connector_coverage_gaps.cpp do not already hit (SERVER_WAVE_GAPS uncovered
// set: 77 79 184-186 198-199 223-225 270-271 455).
//
// What this file reliably reaches:
//   * 455 -- the /browse std::sort comparator body: a directory holding >=3
//     subdirectories forces the comparator to run on distinct elements, and we
//     assert the response is alphabetically sorted.
//   * 77,79 -- ConnectorEnsureRegistry's LoadState non-ok WARN arm: best-effort.
//     We seed a structurally-broken watchers.json and call EnsureRegistry. If the
//     registry's LoadState surfaces a non-ok Status the WARN arm runs; if it
//     swallows the error to Ok (the documented "corrupt file is non-fatal"
//     contract) the arm stays unreached -- either way EnsureRegistry must not throw
//     and must still produce a usable registry. This is asserted, not the warn.
//
// Honestly NOT reachable from an HTTP unit test (documented, not attempted as
// passing assertions on the arm itself):
//   * 184-186 / 223-225 -- the empty-`:id` (and empty-`:ns`) 400 guards. httplib's
//     ":id" path pattern only matches a NON-empty path segment, so a request to
//     ".../watchers/" (or ".../namespaces/") never dispatches into the handler;
//     the empty-id guard is dead defensive code from the route layer's view.
//   * 198-199 -- RemoveWatch's post-resolve error arm. DirectoryForId(id) has
//     already resolved the watcher, so RemoveWatch(*dir) on that same live dir
//     returns Ok; making it fail needs a store/registry failure seam the public
//     API does not expose.
//   * 270-271 -- TriggerScan's post-resolve error arm, same reasoning as 198-199.
//
// Suite prefix ConnectorRoutesFull (globally unique). Reuses the NsPoolHarness +
// MockNSRouter wiring established by test_connector_routes.cpp.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/connector/dir_watcher_registry.h"
#include "cortrix/server/routes/connector_routes.h"
#include "cortrix/spc/spc_manager.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string UniqueSuffix() {
    static std::atomic<int> ctr{0};
    return std::to_string(::getpid()) + "_" + std::to_string(ctr.fetch_add(1));
}

class StubSPCFull : public SPCManager {
public:
    StubSPCFull() : SPCManager() {}
    Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
    int CancelBySourcePath(const std::string&) override { return 0; }
    void Start() override {}
    void Stop() override {}
    size_t QueueSize() const override { return 0; }
    SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
};

class ConnectorRoutesFull : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() / ("cortrix_conn_full_" + UniqueSuffix());
        fs::create_directories(temp_dir_);
        connector_state_.data_dir = (temp_dir_ / "state").string();
        fs::create_directories(connector_state_.data_dir);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());
        using ::testing::_;
        using ::testing::Return;
        ON_CALL(ns_router_, CreateNamespace(_)).WillByDefault(Return(Status::Ok()));

        spc_ = std::make_unique<StubSPCFull>();
        auth_ = std::make_unique<ApiKeyAuth>();
        auth_->set_enabled(false);  // no-auth CE mode: WithAuth grants admin

        static std::atomic<unsigned> ctr{0};
        port_ = 19200 + ((::getpid() + ctr.fetch_add(1)) % 200);
        server_ = std::make_unique<httplib::Server>();
        RegisterConnectorRoutes(*server_, connector_state_, harness_->ipool(),
                                ns_router_, *spc_, *auth_);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });

        httplib::Client probe("127.0.0.1", port_);
        for (int i = 0; i < 60; ++i) {
            auto r = probe.Get("/api/v1/connector/watchers");
            if (r) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    fs::path temp_dir_;
    ConnectorState connector_state_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router_;
    std::unique_ptr<StubSPCFull> spc_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
};

// --- 455: /browse sort comparator over >=3 distinct subdirectories ---
TEST_F(ConnectorRoutesFull, BrowseSortsManySubdirs) {
    fs::path d = temp_dir_ / "browse_root";
    fs::create_directories(d);
    // Create them out of order so the sort must actually reorder.
    for (const char* name : {"mango", "apple", "zebra", "cherry", "banana"}) {
        fs::create_directories(d / name);
    }

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/browse?path=" + d.string());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto entries = json::parse(res->body)["entries"];
    ASSERT_GE(entries.size(), 5u) << res->body;
    for (size_t i = 1; i < entries.size(); ++i) {
        EXPECT_LE(entries[i - 1]["name"].get<std::string>(),
                  entries[i]["name"].get<std::string>());
    }
    // First/last pin the comparator actually ran.
    EXPECT_EQ(entries.front()["name"], "apple");
    EXPECT_EQ(entries.back()["name"], "zebra");
}

}  // namespace

// --- 77,79: ConnectorEnsureRegistry LoadState non-ok WARN arm (best-effort) ---
// Standalone (no fixture) so we control watchers.json before the registry builds.
namespace {

TEST(ConnectorRoutesFullEnsure, CorruptWatchersJsonStillBuildsRegistry) {
    auto tmp = fs::temp_directory_path() / ("cortrix_conn_corrupt_" + UniqueSuffix());
    fs::create_directories(tmp);
    auto state_dir = tmp / "state";
    fs::create_directories(state_dir);

    // A structurally-broken watchers.json: valid JSON, wrong shape (watchers is an
    // object, version is a string). Whether LoadState returns non-ok (-> WARN arm
    // 77-79) or swallows to Ok is an internal contract; we only require that
    // EnsureRegistry tolerates it and yields a usable, empty registry.
    {
        std::ofstream f(state_dir / "watchers.json");
        f << R"({"version":"not-a-number","watchers":{"oops":true}})";
    }

    cortrix::test::NsPoolHarness harness(tmp / "pool");
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router;
    using ::testing::_;
    using ::testing::Return;
    ON_CALL(ns_router, CreateNamespace(_)).WillByDefault(Return(Status::Ok()));
    StubSPCFull spc;

    ConnectorState state;
    state.data_dir = state_dir.string();

    ASSERT_NO_THROW(ConnectorEnsureRegistry(state, harness.ipool(), ns_router, spc));
    ASSERT_NE(state.registry, nullptr);
    // A broken file restores zero watchers (non-fatal, documented behavior).
    EXPECT_TRUE(state.registry->ListWatches().empty());

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// Also exercise the totally-unparseable case (raw garbage, not even JSON).
TEST(ConnectorRoutesFullEnsure, UnparseableWatchersJsonNonFatal) {
    auto tmp = fs::temp_directory_path() / ("cortrix_conn_garbage_" + UniqueSuffix());
    fs::create_directories(tmp);
    auto state_dir = tmp / "state";
    fs::create_directories(state_dir);
    { std::ofstream(state_dir / "watchers.json") << "}{ this is not json at all %%%"; }

    cortrix::test::NsPoolHarness harness(tmp / "pool");
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router;
    using ::testing::_;
    using ::testing::Return;
    ON_CALL(ns_router, CreateNamespace(_)).WillByDefault(Return(Status::Ok()));
    StubSPCFull spc;

    ConnectorState state;
    state.data_dir = state_dir.string();
    ASSERT_NO_THROW(ConnectorEnsureRegistry(state, harness.ipool(), ns_router, spc));
    ASSERT_NE(state.registry, nullptr);
    EXPECT_TRUE(state.registry->ListWatches().empty());

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

}  // namespace
}  // namespace cortrix
