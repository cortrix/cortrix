// Full-surface coverage top-up for the nested document routes
// (src/server/routes/document_routes.cpp). The pre-existing test_document_routes.cpp
// + test_document_routes_extra.cpp cover the upload/list/delete happy paths, the
// 401/403/404 gates, the 413 (file-too-large) and 507 (disk-full) arms and the
// pagination edges. This file targets the branches gcov still reports uncovered
// (SERVER_WAVE_GAPS):
//
//   * DocStatusToString switch arms (error / stale via list; processing / deleting
//     via the status endpoint) — the list/status paths reshape doc.status,
//   * MapStatusToHttp 415 arm ("unsupported media type") and 503 arm
//     (kUnavailable, "processing queue full") via a real UploadHandler over an SPC
//     stub that rejects a supported upload,
//   * the list block_count==0 -> block_count_by_doc(...) fill-in arm over a seeded
//     zero-block doc,
//   * the "multiple files received" warn arm (req.files.count("file") > 1).
//
// Harness mirrors test_document_routes.cpp (real namespace pool NsPoolHarness + real
// UploadHandler + real PermissionService authz seam + in-process httplib server).
// Suite/fixture names are module-prefixed (DocRoutesFull) for global gtest
// uniqueness. Deterministic: 127.0.0.1 only, no LLM/network/model.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/common/data_types.h"
#include "cortrix/config/config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/http_server.h"
#include "cortrix/server/routes/document_routes.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/upload/upload_handler.h"

#include "ns_pool_test_helper.h"
#include "unit/namespace_authz_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// An SPC stub whose Submit() can be made to fail, so HandleUpload returns
// Status::Unavailable("processing queue full") -> MapStatusToHttp -> 503.
class FailableSPC : public SPCManager {
public:
    FailableSPC() : SPCManager() {}
    Status Submit(std::shared_ptr<SPCTask>) override {
        return submit_fail_
                   ? Status(StatusCode::kUnavailable, "queue saturated")
                   : Status::Ok();
    }
    int CancelBySourcePath(const std::string&) override { return 0; }
    void Start() override {}
    void Stop() override {}
    size_t QueueSize() const override { return 0; }
    SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    bool submit_fail_ = false;
};

class DocRoutesFull : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() /
                    ("cortrix_docfull_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_);

        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        auth_ = std::make_unique<ApiKeyAuth>();
        test_key_ = "docfull-key";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key_);
        kc.tenant_id = "t1";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({kc});

        authz_ = std::make_unique<cortrix::test::NamespaceAuthzHarness>(
            *auth_, &harness_->pool(), "t1", std::vector<std::string>{"default"});

        spc_ = std::make_unique<FailableSPC>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_);

        static int port_seed = 0;
        port_ = 18900 + ((getpid() + port_seed++) % 300);
        server_ = std::make_unique<httplib::Server>();
        RegisterDocumentRoutes(*server_, *handler_, harness_->ipool(), *auth_);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });

        httplib::Client probe("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto r = probe.Get("/api/v1/namespaces/default/documents", AuthHeaders());
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

    httplib::Headers AuthHeaders() { return {{"Authorization", "Bearer " + test_key_}}; }

    // Seed a doc directly into the per-NS store with a chosen status + block_count.
    std::string SeedDoc(DocStatus status, const std::string& source_path,
                        int64_t block_count = 0) {
        resource::NamespaceFacade facade(harness_->ipool(), "default");
        EXPECT_TRUE(facade.Acquire().ok());
        CortrixDoc doc;
        doc.source_type = "http_upload";
        doc.source_path = source_path;
        doc.content_hash = "h_" + source_path;
        doc.status = status;
        doc.block_count = block_count;
        EXPECT_EQ(facade.store().doc_create(doc), 0);
        return doc.doc_id;
    }

    CortrixConfig config_;
    fs::path temp_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<cortrix::test::NamespaceAuthzHarness> authz_;
    std::unique_ptr<FailableSPC> spc_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string test_key_;
};

// ---------------------------------------------------------------------------
// DocStatusToString switch arms.
// ---------------------------------------------------------------------------

// list path (lines reshaping each non-deleted status) -> error / stale tokens.
TEST_F(DocRoutesFull, ListReshapesErrorAndStale) {
    SeedDoc(DocStatus::kError, "err.txt");
    SeedDoc(DocStatus::kStale, "stale.txt");
    SeedDoc(DocStatus::kProcessing, "proc.txt");

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/namespaces/default/documents", AuthHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    ASSERT_EQ(j["total_count"], 3);
    std::map<std::string, std::string> by_file;
    for (const auto& d : j["documents"]) {
        by_file[d["source_path"].get<std::string>()] = d["status"].get<std::string>();
    }
    EXPECT_EQ(by_file["err.txt"], "error");
    EXPECT_EQ(by_file["stale.txt"], "stale");
    EXPECT_EQ(by_file["proc.txt"], "processing");
}

// status endpoint reshapes a kDeleting doc to "deleting" (only kDeleted is filtered
// to 404; kDeleting is still externally visible and reshaped).
TEST_F(DocRoutesFull, StatusReshapesDeleting) {
    std::string id = SeedDoc(DocStatus::kDeleting, "deleting.txt");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/namespaces/default/documents/" + id + "/status",
                       AuthHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["status"], "deleting");
}

// status endpoint reshapes a kError doc to "error".
TEST_F(DocRoutesFull, StatusReshapesError) {
    std::string id = SeedDoc(DocStatus::kError, "status_err.txt");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/namespaces/default/documents/" + id + "/status",
                       AuthHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["status"], "error");
}

// ---------------------------------------------------------------------------
// List block_count==0 -> block_count_by_doc(...) fill-in arm.
// ---------------------------------------------------------------------------

TEST_F(DocRoutesFull, ListZeroBlockCountQueriesBlockTable) {
    SeedDoc(DocStatus::kReady, "zeroblk.txt", /*block_count=*/0);
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/namespaces/default/documents", AuthHeaders());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    ASSERT_EQ(j["documents"].size(), 1u);
    // The doc had block_count 0 so the route queried block_count_by_doc; no blocks
    // were inserted, so the surfaced count stays 0.
    EXPECT_EQ(j["documents"][0]["block_count"], 0);
}

// ---------------------------------------------------------------------------
// MapStatusToHttp arms: 415 (unsupported media type) + 503 (queue full).
// ---------------------------------------------------------------------------

TEST_F(DocRoutesFull, UploadUnsupportedMediaType415) {
    httplib::Client cli("127.0.0.1", port_);
    // A non-empty, non-octet-stream mime that is NOT in the SPC supported set, and a
    // filename with no recognized extension, so ResolveMimeType keeps it as-is and
    // ValidateMimeType rejects it -> InvalidArgument("unsupported media type") -> 415.
    httplib::MultipartFormDataItems items = {
        {"file", "binary blob", "weird.unknownext", "application/x-not-supported"},
    };
    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 415) << res->body;
}

TEST_F(DocRoutesFull, UploadQueueFull503) {
    spc_->submit_fail_ = true;  // SPC submit fails -> HandleUpload -> kUnavailable
    httplib::Client cli("127.0.0.1", port_);
    httplib::MultipartFormDataItems items = {
        {"file", "queue full content", "qf.txt", "text/plain"},
    };
    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503) << res->body;
}

// ---------------------------------------------------------------------------
// "multiple files received" warn arm (req.files.count("file") > 1).
// ---------------------------------------------------------------------------

TEST_F(DocRoutesFull, UploadMultipleFileFieldsUsesFirst) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::MultipartFormDataItems items = {
        {"file", "first content", "first.txt", "text/plain"},
        {"file", "second content", "second.txt", "text/plain"},
    };
    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    // The handler logs a warning and proceeds with the first file -> a normal 201.
    EXPECT_EQ(res->status, 201) << res->body;
    EXPECT_EQ(json::parse(res->body)["source_path"], "first.txt");
}

}  // namespace
}  // namespace cortrix
