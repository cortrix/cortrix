/// @file test_sec_rbac_matrix.cpp
/// @brief Security matrix A: RBAC role x route-class authorization matrix.
///
/// Drives every protected route class served by CortrixHttpServer::RegisterRoutes()
/// (the CE ApiKeyAuth/WithAuth model) with each principal type and asserts the
/// expected HTTP status:
///   - public/no-auth routes        -> 2xx regardless of credential;
///   - READ-gated routes            -> {no cred|anonymous}=401, {read|write|admin key}=2xx;
///   - ADMIN-gated routes           -> {no cred}=401, {valid NON-admin key}=403 (NOT 200),
///                                     {valid admin key}=2xx.
///
/// The ADMIN matrix is the security-critical one: a valid read-only or write-only
/// key MUST be rejected with 403 on POST/DELETE /namespaces, never silently
/// granted. WithAuth() -> ApiKeyAuth::Authorize() enforces the kPermAdmin bit
/// (src/auth/auth_middleware.cpp + src/auth/api_key_auth.cpp).
///
/// Note on route classes wired here: CortrixHttpServer::RegisterRoutes() mounts the
/// CE core (health/openapi/system/namespace) only. The admin/users, jwt-rotate,
/// smtp-admin, gc and system-config routes are registered by separate D3.5 wiring
/// (RegisterAdminUsersRoutes / RegisterGcRoutes / RegisterSystemConfigRoutes) that
/// is NOT mounted by this server, so they are not reachable through this fixture.
/// The admin/users permission gate is covered standalone in
/// tests/unit/test_admin_users_routes.cpp; here the admin-only class is exercised
/// via POST + DELETE /api/v1/namespaces, which are the kPermAdmin-gated routes this
/// server actually serves.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/config/config.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// A principal under test: a label + the bearer token to present (empty = no cred).
struct Principal {
    std::string label;
    std::string token;  // "" => send no Authorization header
};

class RbacMatrixTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_sec_rbac_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        admin_key_ = "rbac-admin-secret-key";
        read_key_ = "rbac-read-only-secret-key";
        write_key_ = "rbac-write-only-secret-key";

        config_.server.host = "127.0.0.1";
        config_.server.thread_count = 2;
        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_.string();

        // Full-permission (admin) key.
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "rbac-admin-tenant";
        admin_kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        admin_kc.expires_at = 0;
        config_.auth.api_keys.push_back(admin_kc);

        // Read-only key (valid principal, NO admin bit).
        ApiKeyConfig read_kc;
        read_kc.key_hash = ApiKeyAuth::HashKey(read_key_);
        read_kc.tenant_id = "rbac-read-tenant";
        read_kc.permissions = kPermRead;
        read_kc.expires_at = 0;
        config_.auth.api_keys.push_back(read_kc);

        // Write-capable but NON-admin key (read+write, still NO admin bit).
        ApiKeyConfig write_kc;
        write_kc.key_hash = ApiKeyAuth::HashKey(write_key_);
        write_kc.tenant_id = "rbac-write-tenant";
        write_kc.permissions = kPermRead | kPermWrite;
        write_kc.expires_at = 0;
        config_.auth.api_keys.push_back(write_kc);

        // Distinct port band from test_auth_bypass.cpp (19200+) to avoid collision.
        port_ = 19700 + (getpid() % 250);
        config_.server.port = port_;

        ns_mgr_ = std::make_unique<NamespaceManager>(config_.ns.data_dir);
        ns_mgr_->Init();

        auth_.LoadKeys(config_.auth.api_keys);

        server_ = std::make_unique<CortrixHttpServer>(config_, auth_, *ns_mgr_);
        server_->RegisterRoutes();

        server_thread_ = std::thread([this] { server_->Start(); });

        // Wait until the server accepts connections.
        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/health");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void TearDown() override {
        server_->Stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    httplib::Headers HeadersFor(const std::string& token) {
        if (token.empty()) return {};
        return {{"Authorization", "Bearer " + token}};
    }

    // Principals: no-credential (anonymous), valid read-only, valid write (non-admin),
    // valid admin.
    std::vector<Principal> Principals() {
        return {
            {"no-credential", ""},
            {"read-only-key", read_key_},
            {"write-non-admin-key", write_key_},
            {"admin-key", admin_key_},
        };
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<CortrixHttpServer> server_;
    std::thread server_thread_;
    fs::path tmp_dir_;
    int port_ = 0;
    std::string admin_key_;
    std::string read_key_;
    std::string write_key_;
};

// ---------------------------------------------------------------------------
// Public / no-auth route class: 2xx for every principal (incl. no credential).
// ---------------------------------------------------------------------------

TEST_F(RbacMatrixTest, PublicRoutes_AnyPrincipal_2xx) {
    httplib::Client cli("127.0.0.1", port_);
    const std::vector<std::string> public_get = {
        "/api/v1/health",
        "/api/v1/system/features",
    };
    for (const auto& p : Principals()) {
        for (const auto& path : public_get) {
            auto res = cli.Get(path.c_str(), HeadersFor(p.token));
            ASSERT_TRUE(res) << "no response: " << path << " as " << p.label;
            EXPECT_GE(res->status, 200) << path << " as " << p.label;
            EXPECT_LT(res->status, 300)
                << "public route " << path << " must be 2xx for principal '"
                << p.label << "', got " << res->status;
        }
    }
}

// ---------------------------------------------------------------------------
// READ-gated route class (GET /namespaces, GET /namespaces/:name):
//   no credential -> 401; any valid key (read/write/admin) -> 2xx.
// ---------------------------------------------------------------------------

TEST_F(RbacMatrixTest, ReadGated_NoCredential_401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/namespaces");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "UNAUTHORIZED");
}

TEST_F(RbacMatrixTest, ReadGated_ValidKeys_2xx) {
    httplib::Client cli("127.0.0.1", port_);
    // Every valid principal (read/write/admin) may read.
    for (const std::string& key : {read_key_, write_key_, admin_key_}) {
        auto res = cli.Get("/api/v1/namespaces", HeadersFor(key));
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200)
            << "valid key must read /namespaces, got " << res->status << " body=" << res->body;
    }
}

// ---------------------------------------------------------------------------
// ADMIN-gated route class: POST /namespaces (create) + DELETE /namespaces/:name.
// SECURITY-CRITICAL: a valid NON-admin key (read-only OR write-only) MUST get 403,
// never 2xx. No credential -> 401. Admin key -> 2xx.
// ---------------------------------------------------------------------------

TEST_F(RbacMatrixTest, AdminCreate_NoCredential_401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/namespaces", R"({"name":"rbac-ns-anon"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "UNAUTHORIZED");
}

TEST_F(RbacMatrixTest, AdminCreate_ReadOnlyKey_403_NotGranted) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/namespaces", HeadersFor(read_key_),
                        R"({"name":"rbac-ns-read"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403)
        << "read-only key reached an ADMIN route -> RBAC bypass! got " << res->status
        << " body=" << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "FORBIDDEN");
}

TEST_F(RbacMatrixTest, AdminCreate_WriteNonAdminKey_403_NotGranted) {
    httplib::Client cli("127.0.0.1", port_);
    // A write-capable but non-admin key still must NOT create a namespace.
    auto res = cli.Post("/api/v1/namespaces", HeadersFor(write_key_),
                        R"({"name":"rbac-ns-write"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403)
        << "write (non-admin) key reached an ADMIN route -> RBAC bypass! got "
        << res->status << " body=" << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "FORBIDDEN");
}

TEST_F(RbacMatrixTest, AdminCreate_AdminKey_2xx) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/namespaces", HeadersFor(admin_key_),
                        R"({"name":"rbac-ns-admin"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 200) << res->body;
    EXPECT_LT(res->status, 300)
        << "admin key must create namespace, got " << res->status << " body=" << res->body;
}

TEST_F(RbacMatrixTest, AdminDelete_NoCredential_401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/namespaces/whatever");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401) << res->body;
}

TEST_F(RbacMatrixTest, AdminDelete_ReadOnlyKey_403_NotGranted) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/namespaces/whatever", HeadersFor(read_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403)
        << "read-only key reached ADMIN DELETE -> RBAC bypass! got " << res->status
        << " body=" << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "FORBIDDEN");
}

TEST_F(RbacMatrixTest, AdminDelete_WriteNonAdminKey_403_NotGranted) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/namespaces/whatever", HeadersFor(write_key_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403)
        << "write (non-admin) key reached ADMIN DELETE -> RBAC bypass! got "
        << res->status << " body=" << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "FORBIDDEN");
}

// ---------------------------------------------------------------------------
// Full matrix driver: exhaustively cross every principal with the ADMIN route
// class and assert the exact expected status from the matrix. This is the
// single source-of-truth assertion that no non-admin principal ever reaches an
// admin-only route with a 2xx.
// ---------------------------------------------------------------------------

TEST_F(RbacMatrixTest, AdminRouteClass_FullPrincipalMatrix) {
    httplib::Client cli("127.0.0.1", port_);
    struct Expect {
        std::string principal_label;
        std::string token;
        int want_status;  // 401 (no cred) | 403 (non-admin) | 2xx-marker 201
    };
    // 201 marker: create returns 201 on success; we only assert <300 for the admin row.
    std::vector<Expect> rows = {
        {"no-credential", "", 401},
        {"read-only-key", read_key_, 403},
        {"write-non-admin-key", write_key_, 403},
        {"admin-key", admin_key_, 201},
    };
    int i = 0;
    for (const auto& row : rows) {
        const std::string ns_name = "rbac-matrix-ns-" + std::to_string(i++);
        const std::string payload = std::string("{\"name\":\"") + ns_name + "\"}";
        auto res = cli.Post("/api/v1/namespaces", HeadersFor(row.token), payload,
                            "application/json");
        ASSERT_TRUE(res) << "no response for principal " << row.principal_label;
        if (row.want_status == 201) {
            EXPECT_GE(res->status, 200) << row.principal_label << " body=" << res->body;
            EXPECT_LT(res->status, 300)
                << "admin principal must succeed, got " << res->status;
        } else {
            EXPECT_EQ(res->status, row.want_status)
                << "principal '" << row.principal_label
                << "' on ADMIN route: expected " << row.want_status << ", got "
                << res->status << " body=" << res->body;
        }
    }
}

}  // namespace
}  // namespace cortrix
