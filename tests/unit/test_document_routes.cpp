#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/server/routes/document_routes.h"
#include "cortrix/upload/upload_handler.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/config/config.h"
#include "cortrix/server/http_server.h"
#include "cortrix/common/data_types.h"

// integration wire⑤c: document routes now take a resource::INamespacePool& (was the MVP
// CortrixNamespaceManager&). The shared harness stands up a real DefaultNamespacePool
// (mocked catalog routers + FakeIndex + real WriteCoordinator over a temp dir).
#include "ns_pool_test_helper.h"
// [V6] Runtime namespace authorization seam over a real PermissionService
// (the static per-key allow-list / can_access_namespace was removed).
#include "unit/namespace_authz_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// A stub SPCManager for testing without real processing
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

class DocumentRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() / ("cortrix_docroute_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_);

        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;

        // Real namespace pool over a temp dir; admit the "default" namespace the route
        // tests target so per-request facade.Acquire() succeeds.
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        // Set up auth with a test API key
        auth_ = std::make_unique<ApiKeyAuth>();
        test_key_ = "docroute-test-key";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key_);
        kc.tenant_id = "test-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({kc});

        // Read-only key (different tenant; its only test asserts the WRITE perm
        // gate fires with 403 before namespace authz is ever consulted).
        read_key_ = "docroute-readonly-key";
        ApiKeyConfig ro_kc;
        ro_kc.key_hash = ApiKeyAuth::HashKey(read_key_);
        ro_kc.tenant_id = "ro-tenant";
        ro_kc.permissions = kPermRead;

        // Load both keys
        auth_->LoadKeys({kc, ro_kc});

        // [V6] Real PermissionService over an in-memory catalog. Seed every
        // namespace the tests reach so the admin key (tenant "test-tenant") is an
        // authorized principal. Only "default" is admitted into the namespace pool; the
        // others ("nonexistent", "nonexistent-ns", "ghost-ns") are authorized but
        // absent from the pool, so the facade miss yields the expected 404
        // NOT_FOUND (a 404 for an authorized caller, not an anti-enumeration 403).
        authz_ = std::make_unique<cortrix::test::NamespaceAuthzHarness>(
            *auth_, &harness_->pool(), "test-tenant",
            std::vector<std::string>{"default", "nonexistent", "nonexistent-ns",
                                     "ghost-ns"});

        spc_ = std::make_unique<StubSPCManager>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_);

        port_ = 19300 + (getpid() % 500);
        server_ = std::make_unique<httplib::Server>();
        RegisterDocumentRoutes(*server_, *handler_, harness_->ipool(), *auth_);

        server_thread_ = std::thread([this]() {
            server_->listen("127.0.0.1", port_);
        });

        // Wait for server
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + test_key_}};
    }

    httplib::Headers ReadOnlyHeaders() {
        return {{"Authorization", "Bearer " + read_key_}};
    }

    CortrixConfig config_;
    fs::path temp_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<cortrix::test::NamespaceAuthzHarness> authz_;
    std::unique_ptr<StubSPCManager> spc_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_;
    std::string test_key_;
    std::string read_key_;
};

// ---------------------------------------------------------------------------
// Upload endpoint tests
// ---------------------------------------------------------------------------

TEST_F(DocumentRoutesTest, UploadSuccess) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "Hello World", "test.txt", "text/plain"},
    };

    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201) << res->body;

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("doc_id"));
    EXPECT_TRUE(body.contains("content_hash"));
    EXPECT_EQ(body["status"], "pending");
    EXPECT_EQ(body["source_path"], "test.txt");
    EXPECT_TRUE(body.contains("message"));
}

// R9 robustness: a multipart upload whose "metadata" field is deeply-nested JSON must
// be rejected at ingest (422), not stored verbatim to crash the SPC worker / query
// flatten later (the persistent poisoned-doc DoS). The field is stored verbatim here,
// so the guard parses it and rejects on excessive depth.
TEST_F(DocumentRoutesTest, UploadDeepMetadataRejected422) {
    httplib::Client cli("127.0.0.1", port_);

    std::string deep;
    for (int i = 0; i < 5000; ++i) deep += "{\"a\":";
    deep += "1";
    for (int i = 0; i < 5000; ++i) deep += "}";

    httplib::MultipartFormDataItems items = {
        {"file", "Hello World", "test.txt", "text/plain"},
        {"metadata", deep, "", "application/json"},
    };
    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res) << "server crashed / no response on deep metadata";
    EXPECT_EQ(res->status, 422) << res->body;
    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"]["code"], "CX_ERR_METADATA_TOO_DEEP");
}

// Shallow JSON metadata in a multipart upload is accepted (guard does not over-reject).
TEST_F(DocumentRoutesTest, UploadShallowMetadataAccepted) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::MultipartFormDataItems items = {
        {"file", "Hello World", "test.txt", "text/plain"},
        {"metadata", R"({"tag":"unit","n":3})", "", "application/json"},
    };
    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201) << res->body;
}

TEST_F(DocumentRoutesTest, UploadNoAuth) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "content", "test.txt", "text/plain"},
    };

    auto res = cli.Post("/api/v1/namespaces/default/documents", items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(DocumentRoutesTest, UploadReadOnlyKeyForbidden) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "content", "test.txt", "text/plain"},
    };

    auto res = cli.Post("/api/v1/namespaces/default/documents", ReadOnlyHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

TEST_F(DocumentRoutesTest, UploadNamespaceNotFound) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "content", "test.txt", "text/plain"},
    };

    auto res = cli.Post("/api/v1/namespaces/nonexistent/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

TEST_F(DocumentRoutesTest, UploadMissingFileField) {
    httplib::Client cli("127.0.0.1", port_);

    // Send a multipart request without a "file" field
    httplib::MultipartFormDataItems items = {
        {"metadata", R"({"tag":"test"})", "", "application/json"},
    };

    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
    EXPECT_TRUE(body["error"]["message"].get<std::string>().find("file") != std::string::npos);
}

TEST_F(DocumentRoutesTest, UploadEmptyFilename) {
    httplib::Client cli("127.0.0.1", port_);

    // File with empty filename
    httplib::MultipartFormDataItems items = {
        {"file", "content", "", "text/plain"},
    };

    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(DocumentRoutesTest, UploadDuplicateSkipped) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "same content", "dup.txt", "text/plain"},
    };

    // First upload
    auto res1 = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res1);
    EXPECT_EQ(res1->status, 201);

    // Second upload with same content
    auto res2 = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 200);

    auto body2 = json::parse(res2->body);
    EXPECT_EQ(body2["status"], "skipped");
    EXPECT_TRUE(body2.contains("message"));
}

TEST_F(DocumentRoutesTest, UploadContentChanged) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items1 = {
        {"file", "Version 1", "changeme.txt", "text/plain"},
    };
    auto res1 = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items1);
    ASSERT_TRUE(res1);
    EXPECT_EQ(res1->status, 201);

    httplib::MultipartFormDataItems items2 = {
        {"file", "Version 2 different content", "changeme.txt", "text/plain"},
    };
    auto res2 = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items2);
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 201);

    auto body2 = json::parse(res2->body);
    EXPECT_EQ(body2["status"], "updating");
}

TEST_F(DocumentRoutesTest, UploadWithOptionalTitle) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "content with title", "titled.txt", "text/plain"},
        {"title", "My Document Title", "", ""},
    };

    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);
}

TEST_F(DocumentRoutesTest, UploadWithMetadata) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "content with metadata", "meta.txt", "text/plain"},
        {"metadata", R"({"department":"sales","priority":"high"})", "", ""},
    };

    auto res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);
}

// ---------------------------------------------------------------------------
// Upload error response format compliance (API_GATEWAY_DESIGN.md 2.5.1)
// ---------------------------------------------------------------------------

TEST_F(DocumentRoutesTest, UploadErrorUsesStandardEnvelope) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "content", "test.txt", "text/plain"},
    };

    // Upload to nonexistent namespace to trigger error from handler
    auto res = cli.Post("/api/v1/namespaces/nonexistent-ns/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);

    auto body = json::parse(res->body);
    // Per API_GATEWAY_DESIGN.md 2.5.1: error envelope must have error.code and error.message
    EXPECT_TRUE(body.contains("error"));
    EXPECT_TRUE(body["error"].contains("code"));
    EXPECT_TRUE(body["error"].contains("message"));
}

// ---------------------------------------------------------------------------
// Status endpoint tests
// ---------------------------------------------------------------------------

TEST_F(DocumentRoutesTest, StatusAfterUpload) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "status check content", "status.txt", "text/plain"},
    };

    auto upload_res = cli.Post("/api/v1/namespaces/default/documents", AuthHeaders(), items);
    ASSERT_TRUE(upload_res);
    ASSERT_EQ(upload_res->status, 201);

    auto upload_body = json::parse(upload_res->body);
    std::string doc_id = upload_body["doc_id"];  // D-I6: doc_id is a ULID string

    auto status_res = cli.Get(
        "/api/v1/namespaces/default/documents/" + doc_id + "/status",
        AuthHeaders());
    ASSERT_TRUE(status_res);
    EXPECT_EQ(status_res->status, 200);

    auto status_body = json::parse(status_res->body);
    EXPECT_EQ(status_body["doc_id"], doc_id);
    EXPECT_EQ(status_body["source_type"], "http_upload");
    EXPECT_EQ(status_body["source_path"], "status.txt");
    EXPECT_EQ(status_body["status"], "pending");
    EXPECT_TRUE(status_body.contains("content_hash"));
    EXPECT_TRUE(status_body.contains("mime_type"));
    EXPECT_TRUE(status_body.contains("file_size"));
    EXPECT_TRUE(status_body.contains("created_at"));
    EXPECT_TRUE(status_body.contains("updated_at"));
    EXPECT_TRUE(status_body.contains("processed_at"));
    EXPECT_TRUE(status_body.contains("block_count"));
    EXPECT_TRUE(status_body.contains("error_message"));
}

TEST_F(DocumentRoutesTest, StatusDocNotFound) {
    httplib::Client cli("127.0.0.1", port_);

    auto res = cli.Get("/api/v1/namespaces/default/documents/999999/status", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(DocumentRoutesTest, StatusInvalidDocId) {
    // D-I6: doc_id is a ULID string with no numeric format check, so "abc" is a
    // syntactically valid doc_id that simply isn't found -> 404 (no longer 400).
    httplib::Client cli("127.0.0.1", port_);

    auto res = cli.Get("/api/v1/namespaces/default/documents/abc/status", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(DocumentRoutesTest, StatusDocIdWithTrailingChars) {
    // D-I6: "123abc" is a valid string doc_id (no numeric parse), just not found.
    httplib::Client cli("127.0.0.1", port_);

    auto res = cli.Get("/api/v1/namespaces/default/documents/123abc/status", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(DocumentRoutesTest, StatusNamespaceNotFound) {
    httplib::Client cli("127.0.0.1", port_);

    auto res = cli.Get("/api/v1/namespaces/nonexistent/documents/1/status", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(DocumentRoutesTest, StatusNoAuth) {
    httplib::Client cli("127.0.0.1", port_);

    auto res = cli.Get("/api/v1/namespaces/default/documents/1/status");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(DocumentRoutesTest, StatusNegativeDocId) {
    httplib::Client cli("127.0.0.1", port_);

    // Negative ID should parse fine as int64 but doc won't be found
    auto res = cli.Get("/api/v1/namespaces/default/documents/-1/status", AuthHeaders());
    ASSERT_TRUE(res);
    // Either 400 or 404 is acceptable
    EXPECT_TRUE(res->status == 400 || res->status == 404);
}

TEST_F(DocumentRoutesTest, StatusVeryLargeDocId) {
    httplib::Client cli("127.0.0.1", port_);

    auto res = cli.Get("/api/v1/namespaces/default/documents/9999999999999999999/status",
                       AuthHeaders());
    ASSERT_TRUE(res);
    // Should either parse as very large int or fail with invalid doc_id
    EXPECT_TRUE(res->status == 400 || res->status == 404);
}

// ---------------------------------------------------------------------------
// MapStatusToHttp and MapErrorCode edge cases (tested via integration)
// ---------------------------------------------------------------------------

TEST_F(DocumentRoutesTest, UploadErrorMapsCorrectHttpCodes) {
    // This test verifies the MapStatusToHttp function through the route handler.
    // We test the "namespace not found" path which maps to 404.
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "content", "test.txt", "text/plain"},
    };

    auto res = cli.Post("/api/v1/namespaces/ghost-ns/documents", AuthHeaders(), items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

}  // namespace
}  // namespace cortrix
