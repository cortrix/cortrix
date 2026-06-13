#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/async/document_task_handler.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/config/config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/server/i_task_submitter.h"
#include "cortrix/connector/directory_importer.h"
#include "cortrix/server/routes/connector_routes.h"
#include "cortrix/server/routes/flat_document_routes.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/upload/upload_handler.h"

// Wave S routes coverage: the flat design-surface /documents family
// (POST/GET/GET{id}/DELETE{id} + tasks progress/cancel). The POST path fans through
// a real BatchSubmitService over a stub submitter; the {id}/list paths run over a
// real F05 NsPoolHarness; the task paths run over a real DocumentTaskHandler. Mirrors
// the test_document_routes / test_batch_routes real-httplib-server pattern.
#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using server::BatchSubmitService;
using server::ITaskSubmitter;

// A stub submitter: every doc submits OK and gets a deterministic task id, so the
// batch-of-1 POST /documents path can assert results[0].task_id.
class StubTaskSubmitter : public ITaskSubmitter {
public:
    Result<async::TaskInfo> Submit(const async::SubmitRequest& req) override {
        async::TaskInfo t;
        t.task_id = "task_" + req.doc_id;
        t.doc_id = req.doc_id;
        last_namespace_ = req.namespace_id;
        return t;
    }
    std::string last_namespace_;
};

class StubSPCManager : public SPCManager {
public:
    StubSPCManager() : SPCManager() {}
    Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
    int CancelBySourcePath(const std::string&) override { return 0; }
    void Start() override {}
    void Stop() override {}
    size_t QueueSize() const override { return 0; }
    SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
};

class FlatDocumentRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() / ("cortrix_flatdoc_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_);

        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        auth_ = std::make_unique<ApiKeyAuth>();
        write_key_ = "flat-write-key";
        read_key_ = "flat-read-key";
        ApiKeyConfig wk;
        wk.key_hash = ApiKeyAuth::HashKey(write_key_);
        wk.tenant_id = "t-write";
        wk.permissions = kPermRead | kPermWrite | kPermAdmin;
        ApiKeyConfig rk;
        rk.key_hash = ApiKeyAuth::HashKey(read_key_);
        rk.tenant_id = "t-read";
        rk.permissions = kPermRead;
        auth_->LoadKeys({wk, rk});

        spc_ = std::make_unique<StubSPCManager>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_);

        submitter_ = std::make_unique<StubTaskSubmitter>();
        batch_service_ = std::make_unique<BatchSubmitService>(submitter_.get());

        ASSERT_TRUE(task_mgr_.Init(":memory:").ok());
        scheduler_ = std::make_unique<async::TaskScheduler>(&task_mgr_, &cfg_);
        task_handler_ = std::make_unique<async::DocumentTaskHandler>(
            scheduler_.get(), &task_mgr_, /*pool=*/nullptr, &cfg_);

        // /watch aliases need an INSRouter (CreateNamespace) — a local mock that
        // admits any NS so ConnectorAddWatcher resolves the per-op façade.
        using ::testing::_;
        using ::testing::Return;
        ON_CALL(ns_router_, CreateNamespace(_))
            .WillByDefault(Return(Status::Ok()));
        connector_state_.data_dir = (temp_dir_ / "connector").string();
        fs::create_directories(connector_state_.data_dir);
        watch_dir_ = temp_dir_ / "watched";
        fs::create_directories(watch_dir_);

        port_ = 19900 + (getpid() % 400);
        server_ = std::make_unique<httplib::Server>();
        RegisterFlatDocumentRoutes(*server_, harness_->ipool(), *handler_,
                                   *batch_service_, *task_handler_, *auth_);
        RegisterWatchAliasRoutes(*server_, connector_state_, harness_->ipool(),
                                 ns_router_, *spc_, *auth_);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    httplib::Headers WriteHeaders() { return {{"Authorization", "Bearer " + write_key_}}; }
    httplib::Headers ReadHeaders() { return {{"Authorization", "Bearer " + read_key_}}; }

    // Upload a document straight through the live nested handler so the per-NS store
    // has a row for the GET/DELETE {id} tests, returning its doc_id.
    std::string UploadDoc(const std::string& ns, const std::string& content,
                          const std::string& filename) {
        resource::NamespaceFacade facade(harness_->ipool(), ns);
        EXPECT_TRUE(facade.Acquire().ok());
        UploadRequest req;
        req.namespace_name = ns;
        req.filename = filename;
        req.mime_type = "text/plain";
        req.file_data = content.data();
        req.file_size = content.size();
        UploadResult result;
        Status s = handler_->HandleUpload(req, facade.store(), facade.blob(), &result);
        EXPECT_TRUE(s.ok()) << s.message();
        return result.doc_id;
    }

    CortrixConfig config_;
    fs::path temp_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<StubSPCManager> spc_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<StubTaskSubmitter> submitter_;
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
    int port_;
    std::string write_key_;
    std::string read_key_;
};

// ---------------------------------------------------------------------------
// POST /documents
// ---------------------------------------------------------------------------

TEST_F(FlatDocumentRoutesTest, UploadReturns202DocumentTask) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"namespace", "default"}, {"content", "hello flat world"},
                 {"filename", "a.txt"}};
    auto res = cli.Post("/api/v1/documents", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 202) << res->body;
    auto j = json::parse(res->body);
    EXPECT_TRUE(j.contains("task_id"));        // spec DocumentTask required field
    EXPECT_EQ(j["namespace"], "default");
    EXPECT_EQ(j["status"], "queued");
    EXPECT_EQ(submitter_->last_namespace_, "default");  // fanned to the right NS
}

TEST_F(FlatDocumentRoutesTest, UploadMissingNamespace400) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"content", "no namespace"}};
    auto res = cli.Post("/api/v1/documents", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(FlatDocumentRoutesTest, UploadReadKeyForbidden) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"namespace", "default"}, {"content", "x"}};
    auto res = cli.Post("/api/v1/documents", ReadHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// ---------------------------------------------------------------------------
// GET /documents (list)
// ---------------------------------------------------------------------------

TEST_F(FlatDocumentRoutesTest, ListMissingNamespace400WithStructuredData) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents", ReadHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400);
    auto j = json::parse(res->body);
    EXPECT_EQ(j["code"], "CX_ERR_MISSING_PARAM");
    EXPECT_EQ(j["structured_data"]["missing_param"], "namespace");
}

TEST_F(FlatDocumentRoutesTest, ListReturnsSpecShape) {
    UploadDoc("default", "doc one", "one.txt");
    UploadDoc("default", "doc two", "two.txt");

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents?namespace=default", ReadHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    ASSERT_TRUE(j.contains("documents"));
    EXPECT_TRUE(j.contains("total"));  // spec DocumentList.total (not total_count)
    ASSERT_EQ(j["documents"].size(), 2u);
    // spec Document required fields: document_id + status (not doc_id)
    EXPECT_TRUE(j["documents"][0].contains("document_id"));
    EXPECT_TRUE(j["documents"][0].contains("status"));
    EXPECT_EQ(j["documents"][0]["namespace"], "default");
}

// ---------------------------------------------------------------------------
// GET /documents/{id}
// ---------------------------------------------------------------------------

TEST_F(FlatDocumentRoutesTest, GetByIdMissingNamespace400) {
    std::string doc_id = UploadDoc("default", "fetch me", "f.txt");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents/" + doc_id, ReadHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(FlatDocumentRoutesTest, GetByIdReturnsDocument) {
    std::string doc_id = UploadDoc("default", "fetch me", "f.txt");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents/" + doc_id + "?namespace=default", ReadHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["document_id"], doc_id);
    EXPECT_EQ(j["status"], "pending");  // freshly uploaded
}

// ---------------------------------------------------------------------------
// DELETE /documents/{id} — soft-delete semantics
// ---------------------------------------------------------------------------

TEST_F(FlatDocumentRoutesTest, DeleteSoftDeletesAndThen404) {
    std::string doc_id = UploadDoc("default", "delete me", "d.txt");
    httplib::Client cli("127.0.0.1", port_);

    auto del = cli.Delete("/api/v1/documents/" + doc_id + "?namespace=default", WriteHeaders());
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 204);

    // After soft-delete the doc is no longer externally live -> GET 404.
    auto get = cli.Get("/api/v1/documents/" + doc_id + "?namespace=default", WriteHeaders());
    ASSERT_TRUE(get);
    EXPECT_EQ(get->status, 404);
}

TEST_F(FlatDocumentRoutesTest, DeleteMissingNamespace400) {
    std::string doc_id = UploadDoc("default", "x", "x.txt");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/documents/" + doc_id, WriteHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// ---------------------------------------------------------------------------
// Task progress / cancel
// ---------------------------------------------------------------------------

TEST_F(FlatDocumentRoutesTest, TaskProgressForEnqueuedTask) {
    // Enqueue a task directly so it exists in the manager.
    async::SubmitRequest sreq;
    sreq.namespace_id = "default";
    sreq.filename = "t.txt";
    sreq.filepath = "/tmp/t.txt";
    sreq.doc_id = "doc-prog";
    sreq.content_hash = "h-prog";
    auto enq = scheduler_->Enqueue(sreq);
    ASSERT_TRUE(enq.ok());
    const std::string task_id = enq.value().task_id;

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents/tasks/" + task_id + "/progress", ReadHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["task_id"], task_id);
    EXPECT_EQ(j["status"], "queued");
}

TEST_F(FlatDocumentRoutesTest, TaskProgressNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents/tasks/nope/progress", ReadHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto j = json::parse(res->body);
    EXPECT_EQ(j["code"], "CX_ERR_TASK_NOT_FOUND");
}

TEST_F(FlatDocumentRoutesTest, TaskCancelQueuedTask) {
    async::SubmitRequest sreq;
    sreq.namespace_id = "default";
    sreq.filename = "c.txt";
    sreq.filepath = "/tmp/c.txt";
    sreq.doc_id = "doc-cancel";
    sreq.content_hash = "h-cancel";
    auto enq = scheduler_->Enqueue(sreq);
    ASSERT_TRUE(enq.ok());
    const std::string task_id = enq.value().task_id;

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/documents/tasks/" + task_id, WriteHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["task_id"], task_id);
    EXPECT_EQ(j["cancel_requested"], true);
}

// ---------------------------------------------------------------------------
// /watch aliases
// ---------------------------------------------------------------------------

TEST_F(FlatDocumentRoutesTest, WatchAddReturns201Watcher) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"path", watch_dir_.string()},
                 {"target_namespaces", json::array({"default"})}};
    auto res = cli.Post("/api/v1/watch", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 201) << res->body;
    auto j = json::parse(res->body);
    EXPECT_TRUE(j.contains("id"));                       // spec Watcher required field
    // The connector importer may canonicalize the path (macOS /var -> /private/var),
    // so assert it round-trips through the watched directory name rather than the
    // raw input string.
    EXPECT_NE(j["path"].get<std::string>().find("watched"), std::string::npos);
    ASSERT_TRUE(j["target_namespaces"].is_array());
    EXPECT_EQ(j["target_namespaces"][0], "default");
}

TEST_F(FlatDocumentRoutesTest, WatchAddMissingTargetNamespaces400) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"path", watch_dir_.string()}};
    auto res = cli.Post("/api/v1/watch", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(FlatDocumentRoutesTest, WatchAddDirectoryNotFound404) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"path", (temp_dir_ / "does-not-exist").string()},
                 {"target_namespaces", json::array({"default"})}};
    auto res = cli.Post("/api/v1/watch", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(FlatDocumentRoutesTest, WatchListReturnsSpecShape) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"path", watch_dir_.string()},
                 {"target_namespaces", json::array({"default"})}};
    auto add = cli.Post("/api/v1/watch", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(add);
    ASSERT_EQ(add->status, 201) << add->body;

    auto res = cli.Get("/api/v1/watch", ReadHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    ASSERT_TRUE(j.contains("watchers"));
    ASSERT_EQ(j["watchers"].size(), 1u);
    EXPECT_TRUE(j["watchers"][0].contains("id"));
    EXPECT_NE(j["watchers"][0]["path"].get<std::string>().find("watched"),
              std::string::npos);
    EXPECT_TRUE(j["watchers"][0].contains("target_namespaces"));
}

TEST_F(FlatDocumentRoutesTest, WatchDeleteReturns204ThenGone) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"path", watch_dir_.string()},
                 {"target_namespaces", json::array({"default"})}};
    auto add = cli.Post("/api/v1/watch", WriteHeaders(), body.dump(), "application/json");
    ASSERT_TRUE(add);
    ASSERT_EQ(add->status, 201) << add->body;
    const std::string id = json::parse(add->body)["id"];

    auto del = cli.Delete("/api/v1/watch/" + id, WriteHeaders());
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 204);

    auto list = cli.Get("/api/v1/watch", ReadHeaders());
    ASSERT_TRUE(list);
    EXPECT_EQ(json::parse(list->body)["watchers"].size(), 0u);
}

TEST_F(FlatDocumentRoutesTest, WatchDeleteUnknownId404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/watch/nonexistent", WriteHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

}  // namespace
}  // namespace cortrix
