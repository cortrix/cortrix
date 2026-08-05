#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/server/routes/document_routes.h"
#include "cortrix/upload/upload_handler.h"
#include "cortrix/deploy/disk_monitor.h"  // Deployment disk gate on the upload route
#include "ns_pool_test_helper.h"  // Namespace pool NsPoolHarness replaces MVP NamespaceManager
#include "unit/namespace_authz_test_helper.h"  // [V6] runtime NS-authz seam over a real PermissionService
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/config/config.h"

#include <filesystem>
#include <cstdlib>

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// A fake SPCManager that accepts all submissions without real processing
class TestSPCManager : public SPCManager {
public:
    TestSPCManager() : SPCManager() {}

    Status Submit(std::shared_ptr<SPCTask> task) override {
        (void)task;
        return Status::Ok();
    }

    int CancelBySourcePath(const std::string&) override { return 0; }
    void Start() override {}
    void Stop() override {}
    size_t QueueSize() const override { return 0; }
    SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
};

class DocumentUploadApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temp directory
        temp_dir_ = fs::temp_directory_path() / ("cortrix_test_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_);

        // Build config
        config_.ns.data_dir = temp_dir_.string();
        config_.ns.max_active = 10;
        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;
        config_.embedding.dimension = 128;

        // Namespace pool NS resource pool replaces the MVP NamespaceManager +
        // CortrixNamespaceManager (RegisterDocumentRoutes now takes INamespacePool&).
        // Admit "default" so uploads to it succeed (was implicit in the MVP manager).
        harness_ = std::make_unique<test::NsPoolHarness>(temp_dir_);
        ASSERT_TRUE(harness_->Admit("default").ok());

        // Set up auth with a test API key
        auth_ = std::make_unique<ApiKeyAuth>();
        std::string test_key = "test-api-key-12345";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key);
        kc.tenant_id = "test-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({kc});

        // Read-only key
        std::string ro_key = "read-only-key-67890";
        ApiKeyConfig ro_kc;
        ro_kc.key_hash = ApiKeyAuth::HashKey(ro_key);
        ro_kc.tenant_id = "test-tenant-ro";
        ro_kc.permissions = kPermRead;
        auth_->LoadKeys({kc, ro_kc});

        // [V6] Runtime namespace authorization: a real PermissionService over an
        // in-memory catalog seeding the namespaces the admin key's tenant owns.
        // "default" is owned by "test-tenant" (the admin key's tenant), admitted to
        // the pool -> uploads pass. "nonexistent" is seeded as OWNED (so the caller
        // is authorized) but NOT admitted to the pool -> facade miss -> 404 NOT_FOUND
        // for an authorized caller (vs 403 anti-enumeration for a non-owned NS).
        authz_ = std::make_unique<cortrix::test::NamespaceAuthzHarness>(
            *auth_, &harness_->pool(), "test-tenant",
            std::vector<std::string>{"default", "nonexistent"});

        // Set up SPC + upload handler
        spc_ = std::make_unique<TestSPCManager>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_);

        // Deployment disk gate on the upload route. Not Start()ed — tests drive the
        // NORMAL/WARN/CRIT FSM deterministically via CheckWithSample(); the default
        // state is NORMAL so the existing upload tests are unaffected.
        deploy::DiskMonitorConfig dm_cfg;
        dm_cfg.data_dir = temp_dir_.string();
        disk_monitor_ = std::make_unique<deploy::DiskMonitor>(dm_cfg, nullptr);

        // Set up HTTP server with OS-assigned port
        server_ = std::make_unique<httplib::Server>();
        RegisterDocumentRoutes(*server_, *handler_, harness_->ipool(), *auth_,
                               disk_monitor_.get());

        port_ = server_->bind_to_any_port("127.0.0.1");

        server_thread_ = std::thread([this]() {
            server_->listen_after_bind();
        });

        // Wait for server to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }

        // Clean up temp dir
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    std::string MakeMultipartBody(const std::string& filename,
                                    const std::string& content,
                                    const std::string& content_type = "text/plain",
                                    const std::string& title = "",
                                    const std::string& metadata = "") {
        std::string boundary = "----TestBoundary123456";
        std::string body;

        // File field
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";
        body += "Content-Type: " + content_type + "\r\n\r\n";
        body += content + "\r\n";

        // Title field (optional)
        if (!title.empty()) {
            body += "--" + boundary + "\r\n";
            body += "Content-Disposition: form-data; name=\"title\"\r\n\r\n";
            body += title + "\r\n";
        }

        // Metadata field (optional)
        if (!metadata.empty()) {
            body += "--" + boundary + "\r\n";
            body += "Content-Disposition: form-data; name=\"metadata\"\r\n\r\n";
            body += metadata + "\r\n";
        }

        body += "--" + boundary + "--\r\n";
        return body;
    }

    std::string MultipartContentType() {
        return "multipart/form-data; boundary=----TestBoundary123456";
    }

    CortrixConfig config_;
    fs::path temp_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<cortrix::test::NamespaceAuthzHarness> authz_;
    std::unique_ptr<TestSPCManager> spc_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<deploy::DiskMonitor> disk_monitor_;  // upload-route disk gate
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 18080;  // Use a non-standard port for testing
};

TEST_F(DocumentUploadApiTest, Upload_Txt_Success) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "Hello World content", "test.txt", "text/plain"},
    };

    httplib::Headers headers = {
        {"Authorization", "Bearer test-api-key-12345"},
    };

    auto res = cli.Post("/api/v1/namespaces/default/documents", headers, items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201) << res->body;

    auto body = json::parse(res->body);
    EXPECT_TRUE(body.contains("doc_id"));
    EXPECT_TRUE(body.contains("content_hash"));
    EXPECT_EQ(body["status"], "pending");
    EXPECT_EQ(body["source_path"], "test.txt");
}

TEST_F(DocumentUploadApiTest, Upload_NoAuth) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "content", "test.txt", "text/plain"},
    };

    auto res = cli.Post("/api/v1/namespaces/default/documents", items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(DocumentUploadApiTest, Upload_NamespaceNotFound) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "content", "test.txt", "text/plain"},
    };

    httplib::Headers headers = {
        {"Authorization", "Bearer test-api-key-12345"},
    };

    auto res = cli.Post("/api/v1/namespaces/nonexistent/documents", headers, items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(DocumentUploadApiTest, Upload_DuplicateSkip) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "Same content both times", "dup.txt", "text/plain"},
    };

    httplib::Headers headers = {
        {"Authorization", "Bearer test-api-key-12345"},
    };

    // First upload
    auto res1 = cli.Post("/api/v1/namespaces/default/documents", headers, items);
    ASSERT_TRUE(res1);
    EXPECT_EQ(res1->status, 201);

    // Second upload - same content
    auto res2 = cli.Post("/api/v1/namespaces/default/documents", headers, items);
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 200);

    auto body2 = json::parse(res2->body);
    EXPECT_EQ(body2["status"], "skipped");
}

TEST_F(DocumentUploadApiTest, Upload_ContentChanged) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::Headers headers = {
        {"Authorization", "Bearer test-api-key-12345"},
    };

    // First upload
    httplib::MultipartFormDataItems items1 = {
        {"file", "Version 1", "update.txt", "text/plain"},
    };
    auto res1 = cli.Post("/api/v1/namespaces/default/documents", headers, items1);
    ASSERT_TRUE(res1);
    EXPECT_EQ(res1->status, 201);

    // Second upload - different content, same filename
    httplib::MultipartFormDataItems items2 = {
        {"file", "Version 2 with changes", "update.txt", "text/plain"},
    };
    auto res2 = cli.Post("/api/v1/namespaces/default/documents", headers, items2);
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 201);

    auto body2 = json::parse(res2->body);
    EXPECT_EQ(body2["status"], "updating");
}

TEST_F(DocumentUploadApiTest, Status_AfterUpload) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::Headers headers = {
        {"Authorization", "Bearer test-api-key-12345"},
    };

    // Upload
    httplib::MultipartFormDataItems items = {
        {"file", "Status check content", "status.txt", "text/plain"},
    };
    auto upload_res = cli.Post("/api/v1/namespaces/default/documents", headers, items);
    ASSERT_TRUE(upload_res);
    ASSERT_EQ(upload_res->status, 201);

    auto upload_body = json::parse(upload_res->body);
    std::string doc_id = upload_body["doc_id"];  // [D-I6] doc_id is a ULID string now

    // Query status
    auto status_res = cli.Get(
        "/api/v1/namespaces/default/documents/" + doc_id + "/status",
        headers);
    ASSERT_TRUE(status_res);
    EXPECT_EQ(status_res->status, 200);

    auto status_body = json::parse(status_res->body);
    EXPECT_EQ(status_body["doc_id"], doc_id);
    EXPECT_EQ(status_body["source_type"], "http_upload");
    EXPECT_EQ(status_body["status"], "pending");
}

TEST_F(DocumentUploadApiTest, Status_DocNotFound) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::Headers headers = {
        {"Authorization", "Bearer test-api-key-12345"},
    };

    auto res = cli.Get("/api/v1/namespaces/default/documents/99999/status", headers);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

// Deployment: at CRIT the upload endpoint rejects
// BEFORE any byte is written — 507 + the full CX_ERR_DISK_FULL Agent-friendly
// body; when pressure clears (NORMAL) uploads succeed again.
TEST_F(DocumentUploadApiTest, Upload_DiskFull_Rejected507) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers headers = {
        {"Authorization", "Bearer test-api-key-12345"},
    };
    httplib::MultipartFormDataItems items = {
        {"file", "content under disk pressure", "diskfull.txt", "text/plain"},
    };

    // Drive the FSM to CRIT (>= 0.90 default crit threshold).
    deploy::DiskUsage crit;
    crit.total_bytes = 100;
    crit.free_bytes = 5;
    crit.usage_ratio = 0.95;
    disk_monitor_->CheckWithSample(crit);
    ASSERT_TRUE(disk_monitor_->ShouldRejectWrites());

    auto res = cli.Post("/api/v1/namespaces/default/documents", headers, items);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 507) << res->body;

    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_DISK_FULL");
    EXPECT_EQ(body["error"]["retryable"], false);
    EXPECT_EQ(body["error"]["category"], "permanent");
    ASSERT_TRUE(body["error"].contains("structured_data"));
    EXPECT_TRUE(body["error"]["structured_data"].contains("disk_usage_ratio"));
    EXPECT_TRUE(body["error"]["structured_data"].contains("crit_threshold"));

    // Pressure clears → NORMAL → the same upload succeeds (201).
    deploy::DiskUsage normal;
    normal.total_bytes = 100;
    normal.free_bytes = 80;
    normal.usage_ratio = 0.20;
    disk_monitor_->CheckWithSample(normal);
    ASSERT_FALSE(disk_monitor_->ShouldRejectWrites());

    auto res2 = cli.Post("/api/v1/namespaces/default/documents", headers, items);
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 201) << res2->body;
}

TEST_F(DocumentUploadApiTest, Status_InvalidDocId) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::Headers headers = {
        {"Authorization", "Bearer test-api-key-12345"},
    };

    // [D-I6] doc_id is a ULID string now, so "abc" is a syntactically valid (just
    // non-existent) doc_id → 404 not-found, no longer 400 invalid-format (was int64 parse).
    auto res = cli.Get("/api/v1/namespaces/default/documents/abc/status", headers);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

}  // namespace
}  // namespace cortrix
