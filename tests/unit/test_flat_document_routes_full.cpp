// Full-surface coverage top-up for the flat (design-surface) /documents family +
// /watch aliases + the /namespaces/{id}/reload admin route
// (src/server/routes/flat_document_routes.cpp). The pre-existing
// test_flat_document_routes.cpp drives the happy paths and a handful of validation
// arms; this file targets the branches gcov still reports uncovered (SERVER_WAVE_GAPS):
//
//   * DocStatusToSpec switch arms (processing->parsing, ready, error->failed,
//     stale->pending, default->failed) via list/get over seeded docs in each status,
//   * RequireNamespaceParam empty-?namespace= arm (distinct from missing-param),
//   * POST /documents body-validation arms (invalid JSON / non-object / missing
//     content) and the two service-error arms (per-doc submit failure -> 400 from
//     meta.failed[0]; batch-level envelope rejection -> propagated status),
//   * GET/DELETE /documents/{id} namespace-not-found + doc-not-found arms,
//   * list limit/offset query parsing,
//   * /watch invalid-JSON / missing-path / empty-string-targets / recursive arms,
//   * POST /namespaces/{id}/reload happy path (admin recovery).
//
// Harness: an in-process loopback httplib::Server + RegisterFlatDocumentRoutes /
// RegisterWatchAliasRoutes over a real namespace pool NsPoolHarness, a real UploadHandler, a
// real BatchSubmitService (over a selectable stub submitter), and a real
// DocumentTaskHandler. Suite/fixture names are module-prefixed (FlatDocRoutesFull)
// for global gtest uniqueness. Deterministic: 127.0.0.1 only, no LLM/network/model.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/async/document_task_handler.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/common/data_types.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/config/config.h"
#include "cortrix/connector/dir_watcher_registry.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/server/i_task_submitter.h"
#include "cortrix/server/routes/connector_routes.h"
#include "cortrix/server/routes/flat_document_routes.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/upload/upload_handler.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using server::BatchSubmitService;
using server::ITaskSubmitter;

// A submitter whose behavior is switchable: by default every doc submits OK with a
// deterministic task id (the happy/202 path); flip fail_=true and every doc submit
// returns a CX_ERR Status so the POST /documents per-doc failure arm (meta.failed[0]
// -> 400) is reached.
class FlexSubmitter : public ITaskSubmitter {
public:
    Result<async::TaskInfo> Submit(const async::SubmitRequest& req) override {
        if (fail_) {
            return Result<async::TaskInfo>(
                Status(StatusCode::kInvalidArgument,
                       "CX_ERR_INVALID_CONTENT: submission rejected"));
        }
        async::TaskInfo t;
        t.task_id = "task_" + req.doc_id;
        t.doc_id = req.doc_id;
        return t;
    }
    bool fail_ = false;
};

class FlatStubSPC : public SPCManager {
public:
    FlatStubSPC() : SPCManager() {}
    Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
    int CancelBySourcePath(const std::string&) override { return 0; }
    void Start() override {}
    void Stop() override {}
    size_t QueueSize() const override { return 0; }
    SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
};

class FlatDocRoutesFull : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() /
                    ("cortrix_flatfull_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_);

        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        auth_ = std::make_unique<ApiKeyAuth>();
        write_key_ = "flatfull-write-key";
        read_key_ = "flatfull-read-key";
        ApiKeyConfig wk;
        wk.key_hash = ApiKeyAuth::HashKey(write_key_);
        wk.tenant_id = "t-write";
        wk.permissions = kPermRead | kPermWrite | kPermAdmin;
        ApiKeyConfig rk;
        rk.key_hash = ApiKeyAuth::HashKey(read_key_);
        rk.tenant_id = "t-read";
        rk.permissions = kPermRead;
        auth_->LoadKeys({wk, rk});

        spc_ = std::make_unique<FlatStubSPC>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_);

        submitter_ = std::make_unique<FlexSubmitter>();
        batch_service_ = std::make_unique<BatchSubmitService>(submitter_.get());

        ASSERT_TRUE(task_mgr_.Init(":memory:").ok());
        scheduler_ = std::make_unique<async::TaskScheduler>(&task_mgr_, &cfg_);
        task_handler_ = std::make_unique<async::DocumentTaskHandler>(
            scheduler_.get(), &task_mgr_, /*pool=*/nullptr, &cfg_);

        connector_state_.data_dir = (temp_dir_ / "connector").string();
        fs::create_directories(connector_state_.data_dir);
        watch_dir_ = temp_dir_ / "watched";
        fs::create_directories(watch_dir_);

        using ::testing::_;
        using ::testing::Return;
        ON_CALL(ns_router_, CreateNamespace(_)).WillByDefault(Return(Status::Ok()));

        static int port_seed = 0;
        port_ = 19120 + ((getpid() + port_seed++) % 300);
        server_ = std::make_unique<httplib::Server>();
        RegisterFlatDocumentRoutes(*server_, harness_->ipool(), *handler_,
                                   *batch_service_, *task_handler_, *auth_);
        RegisterWatchAliasRoutes(*server_, connector_state_, harness_->ipool(),
                                 ns_router_, *spc_, *auth_);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });

        httplib::Client probe("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto r = probe.Get("/api/v1/watch", ReadHeaders());
            if (r) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    httplib::Headers WriteHeaders() { return {{"Authorization", "Bearer " + write_key_}}; }
    httplib::Headers ReadHeaders() { return {{"Authorization", "Bearer " + read_key_}}; }

    // Seed a document row directly into the per-NS store with a chosen lifecycle
    // status (doc_create mints the ULID + binds status), so the list/get reshaping
    // hits each DocStatusToSpec switch arm. Returns the minted doc_id.
    std::string SeedDoc(const std::string& ns, DocStatus status,
                        const std::string& source_path) {
        resource::NamespaceFacade facade(harness_->ipool(), ns);
        EXPECT_TRUE(facade.Acquire().ok());
        CortrixDoc doc;
        doc.source_type = "http_upload";
        doc.source_path = source_path;
        doc.content_hash = "h_" + source_path;
        doc.status = status;
        EXPECT_EQ(facade.store().doc_create(doc), 0);
        return doc.doc_id;
    }

    CortrixConfig config_;
    fs::path temp_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<FlatStubSPC> spc_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<FlexSubmitter> submitter_;
    std::unique_ptr<BatchSubmitService> batch_service_;
    async::TaskManager task_mgr_;
    InMemoryGlobalConfig cfg_;
    std::unique_ptr<async::TaskScheduler> scheduler_;
    std::unique_ptr<async::DocumentTaskHandler> task_handler_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router_;
    ConnectorState connector_state_;
    fs::path watch_dir_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string write_key_, read_key_;
};

// ---------------------------------------------------------------------------
// DocStatusToSpec switch arms via GET /documents (list) over seeded docs.
// ---------------------------------------------------------------------------

TEST_F(FlatDocRoutesFull, ListReshapesEachLifecycleStatus) {
    SeedDoc("default", DocStatus::kProcessing, "p.txt");  // -> "parsing"
    SeedDoc("default", DocStatus::kReady, "r.txt");       // -> "ready"
    SeedDoc("default", DocStatus::kError, "e.txt");       // -> "failed"
    SeedDoc("default", DocStatus::kStale, "s.txt");       // -> "pending"

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents?namespace=default", ReadHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    ASSERT_EQ(j["total"], 4);

    // Collect the spec status tokens emitted (order is doc_id-desc, so map by file).
    std::map<std::string, std::string> by_file;
    for (const auto& d : j["documents"]) {
        by_file[d["filename"].get<std::string>()] = d["status"].get<std::string>();
    }
    EXPECT_EQ(by_file["p.txt"], "parsing");
    EXPECT_EQ(by_file["r.txt"], "ready");
    EXPECT_EQ(by_file["e.txt"], "failed");
    EXPECT_EQ(by_file["s.txt"], "pending");
}

// GET /documents/{id} for a kReady doc exercises DocToSpecJson + the ready arm.
TEST_F(FlatDocRoutesFull, GetByIdReadyStatus) {
    std::string id = SeedDoc("default", DocStatus::kReady, "ready_one.txt");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents/" + id + "?namespace=default", ReadHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["document_id"], id);
    EXPECT_EQ(j["status"], "ready");
}

// ---------------------------------------------------------------------------
// RequireNamespaceParam empty-?namespace= arm (distinct from missing-param).
// ---------------------------------------------------------------------------

TEST_F(FlatDocRoutesFull, ListEmptyNamespaceParam400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents?namespace=", ReadHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"]["code"], "CX_ERR_MISSING_PARAM");
    EXPECT_EQ(j["error"]["structured_data"]["missing_param"], "namespace");
}

TEST_F(FlatDocRoutesFull, GetByIdEmptyNamespaceParam400) {
    std::string id = SeedDoc("default", DocStatus::kReady, "x.txt");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents/" + id + "?namespace=", ReadHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
}

// ---------------------------------------------------------------------------
// POST /documents body-validation arms.
// ---------------------------------------------------------------------------

TEST_F(FlatDocRoutesFull, PostInvalidJson400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents", WriteHeaders(), "{not json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
}

TEST_F(FlatDocRoutesFull, PostNonObjectBody400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents", WriteHeaders(), "[1,2,3]", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
}

TEST_F(FlatDocRoutesFull, PostMissingContent400) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"namespace", "default"}};  // no content
    auto res = cli.Post("/api/v1/documents", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
}

// ---------------------------------------------------------------------------
// POST /documents service-error arm: per-doc submit failure -> meta.failed[0] -> 400.
// ---------------------------------------------------------------------------

TEST_F(FlatDocRoutesFull, PostPerDocSubmitFailure400) {
    submitter_->fail_ = true;  // every submit returns a CX_ERR Status
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"namespace", "default"}, {"content", "boom"}, {"filename", "f.txt"}};
    auto res = cli.Post("/api/v1/documents", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    // The propagated failure descriptor carries the agent-friendly error envelope
    // (GEN-Agent failure item): a machine-readable error_code, not a free "error".
    auto j = json::parse(res->body);
    EXPECT_TRUE(j.contains("error_code")) << res->body;
    EXPECT_EQ(j.value("error_code", ""), "CX_ERR_INVALID_CONTENT") << res->body;
}

// ---------------------------------------------------------------------------
// GET /documents/{id} + DELETE /documents/{id} not-found arms.
// ---------------------------------------------------------------------------

TEST_F(FlatDocRoutesFull, GetByIdNamespaceNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents/any-id?namespace=ghost-ns", ReadHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
}

TEST_F(FlatDocRoutesFull, GetByIdDocNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents/no-such-doc?namespace=default", ReadHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
}

TEST_F(FlatDocRoutesFull, DeleteByIdNamespaceNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/documents/any-id?namespace=ghost-ns", WriteHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
}

TEST_F(FlatDocRoutesFull, DeleteByIdDocNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/documents/no-such-doc?namespace=default", WriteHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
}

// DELETE a seeded ready doc soft-deletes (204), then GET is 404 (kDeleted arm).
TEST_F(FlatDocRoutesFull, DeleteSeededDocThen404) {
    std::string id = SeedDoc("default", DocStatus::kReady, "del.txt");
    httplib::Client cli("127.0.0.1", port_);
    auto del = cli.Delete("/api/v1/documents/" + id + "?namespace=default", WriteHeaders());
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 204) << del->body;
    auto get = cli.Get("/api/v1/documents/" + id + "?namespace=default", ReadHeaders());
    ASSERT_TRUE(get);
    EXPECT_EQ(get->status, 404);
}

// ---------------------------------------------------------------------------
// GET /documents list limit/offset query parsing.
// ---------------------------------------------------------------------------

TEST_F(FlatDocRoutesFull, ListWithLimitAndOffset) {
    for (int i = 0; i < 3; ++i) SeedDoc("default", DocStatus::kReady,
                                        "lo_" + std::to_string(i) + ".txt");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents?namespace=default&limit=1&offset=1", ReadHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["total"], 3);
    EXPECT_EQ(j["documents"].size(), 1u);
}

// ---------------------------------------------------------------------------
// /watch validation arms (invalid JSON / missing path / empty-string targets /
// recursive flag) + DELETE unknown id.
// ---------------------------------------------------------------------------

TEST_F(FlatDocRoutesFull, WatchAddInvalidJson400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/watch", WriteHeaders(), "{bad", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
}

TEST_F(FlatDocRoutesFull, WatchAddMissingPath400) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"target_namespaces", json::array({"default"})}};
    auto res = cli.Post("/api/v1/watch", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
}

TEST_F(FlatDocRoutesFull, WatchAddAllEmptyTargets400) {
    httplib::Client cli("127.0.0.1", port_);
    // target_namespaces is a non-empty array, but all entries are empty strings, so
    // after the per-element filter the resulting list is empty -> the second arm.
    json body = {{"path", watch_dir_.string()},
                 {"target_namespaces", json::array({"", ""})}};
    auto res = cli.Post("/api/v1/watch", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
}

TEST_F(FlatDocRoutesFull, WatchAddRecursiveFalse201) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"path", watch_dir_.string()},
                 {"target_namespaces", json::array({"default"})},
                 {"recursive", false}};
    auto res = cli.Post("/api/v1/watch", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 201) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["recursive"], false);
}

TEST_F(FlatDocRoutesFull, WatchDeleteUnknownId404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/watch/no-such-watch-id", WriteHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
}

// ---------------------------------------------------------------------------
// POST /namespaces/{id}/reload — admin recovery happy path.
// ---------------------------------------------------------------------------

TEST_F(FlatDocRoutesFull, ReloadNamespaceHappyPath) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/namespaces/default/reload", WriteHeaders(), "", "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["namespace_id"], "default");
    EXPECT_EQ(j["status"], "reloaded");
}

TEST_F(FlatDocRoutesFull, ReloadNamespaceNonAdmin403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/namespaces/default/reload", ReadHeaders(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
}

// ---------------------------------------------------------------------------
// Batch-level envelope rejection arm: a separate fixture builds the
// BatchSubmitService with a tiny payload cap so the batch-of-1 POST is rejected at
// the envelope (status != 200) and the route propagates it verbatim (lines 185-188).
// ---------------------------------------------------------------------------

class FlatDocRoutesFullSmallLimits : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() /
                    ("cortrix_flatfull_sl_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_);

        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        auth_ = std::make_unique<ApiKeyAuth>();
        write_key_ = "flatfull-sl-write-key";
        ApiKeyConfig wk;
        wk.key_hash = ApiKeyAuth::HashKey(write_key_);
        wk.tenant_id = "t-write";
        wk.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({wk});

        spc_ = std::make_unique<FlatStubSPC>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_);

        submitter_ = std::make_unique<FlexSubmitter>();
        server::BatchLimits limits;
        limits.max_payload_bytes = 4;  // any non-trivial content overflows -> 413
        limits.max_doc_bytes = 4;
        batch_service_ = std::make_unique<BatchSubmitService>(submitter_.get(), limits);

        ASSERT_TRUE(task_mgr_.Init(":memory:").ok());
        scheduler_ = std::make_unique<async::TaskScheduler>(&task_mgr_, &cfg_);
        task_handler_ = std::make_unique<async::DocumentTaskHandler>(
            scheduler_.get(), &task_mgr_, /*pool=*/nullptr, &cfg_);

        static int port_seed = 0;
        port_ = 19450 + ((getpid() + port_seed++) % 200);
        server_ = std::make_unique<httplib::Server>();
        RegisterFlatDocumentRoutes(*server_, harness_->ipool(), *handler_,
                                   *batch_service_, *task_handler_, *auth_);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });

        httplib::Client probe("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto r = probe.Get("/api/v1/documents?namespace=default", WriteHeaders());
            if (r) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    httplib::Headers WriteHeaders() { return {{"Authorization", "Bearer " + write_key_}}; }

    CortrixConfig config_;
    fs::path temp_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<FlatStubSPC> spc_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<FlexSubmitter> submitter_;
    std::unique_ptr<BatchSubmitService> batch_service_;
    async::TaskManager task_mgr_;
    InMemoryGlobalConfig cfg_;
    std::unique_ptr<async::TaskScheduler> scheduler_;
    std::unique_ptr<async::DocumentTaskHandler> task_handler_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string write_key_;
};

TEST_F(FlatDocRoutesFullSmallLimits, PostBatchLevelRejectionPropagated) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"namespace", "default"},
                 {"content", "this content exceeds the tiny payload cap"}};
    auto res = cli.Post("/api/v1/documents", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    // The batch envelope rejects (payload too large) and the route surfaces that
    // status + body verbatim (non-202, non-200 partial).
    EXPECT_GE(res->status, 400) << res->body;
    EXPECT_NE(res->status, 202);
    auto j = json::parse(res->body);
    EXPECT_TRUE(j.contains("error")) << res->body;
}

}  // namespace
}  // namespace cortrix
