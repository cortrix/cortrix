#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/middleware/admin_guard.h"

namespace cortrix::middleware {
namespace {

using nlohmann::json;

// ---- IsLoopback ----

TEST(AdminGuardLoopbackTest, Ipv4LoopbackBlock) {
    EXPECT_TRUE(IsLoopback("127.0.0.1"));
    EXPECT_TRUE(IsLoopback("127.0.0.53"));
    EXPECT_TRUE(IsLoopback("127.255.255.255"));
}

TEST(AdminGuardLoopbackTest, Ipv6Loopback) {
    EXPECT_TRUE(IsLoopback("::1"));
    EXPECT_TRUE(IsLoopback("::ffff:127.0.0.1"));
}

TEST(AdminGuardLoopbackTest, NonLoopbackRejected) {
    EXPECT_FALSE(IsLoopback("192.168.1.10"));
    EXPECT_FALSE(IsLoopback("10.0.0.5"));
    EXPECT_FALSE(IsLoopback("8.8.8.8"));
    EXPECT_FALSE(IsLoopback(""));
    EXPECT_FALSE(IsLoopback("128.0.0.1"));  // adjacent to 127/8 but not in it
}

// ---- IsAdminPath (exact prefix) ----

TEST(AdminGuardPathTest, AdminPrefixesMatch) {
    EXPECT_TRUE(IsAdminPath("/api/v1/admin/auth/rotate-jwt-secret"));
    EXPECT_TRUE(IsAdminPath("/api/v1/admin/users/123"));
    EXPECT_TRUE(IsAdminPath("/api/v1/system/tenants/t1/stats"));
    EXPECT_TRUE(IsAdminPath("/api/v1/system/units/u1/stats"));
}

TEST(AdminGuardPathTest, BusinessAndHealthPathsNotAdmin) {
    EXPECT_FALSE(IsAdminPath("/api/v1/query"));
    EXPECT_FALSE(IsAdminPath("/api/v1/namespaces"));
    EXPECT_FALSE(IsAdminPath("/api/v1/system/health/ready"));
    EXPECT_FALSE(IsAdminPath("/api/v1/system/features"));
    EXPECT_FALSE(IsAdminPath("/health"));
    EXPECT_FALSE(IsAdminPath("/metrics"));
}

// ---- IsClientAllowed (policy) ----

TEST(AdminGuardPolicyTest, LoopbackOnlyDefault) {
    AdminGuard guard(AdminGuardConfig{});  // default whitelist 127.0.0.1
    EXPECT_TRUE(guard.IsClientAllowed("127.0.0.1"));
    EXPECT_FALSE(guard.IsClientAllowed("192.168.1.10"));
}

TEST(AdminGuardPolicyTest, ExplicitLoopbackSameAsDefault) {
    AdminGuard guard(AdminGuardConfig{"127.0.0.1", false});
    EXPECT_TRUE(guard.IsClientAllowed("127.0.0.1"));
    EXPECT_FALSE(guard.IsClientAllowed("10.0.0.9"));
}

TEST(AdminGuardPolicyTest, PublicWhitelistAllowsAll) {
    AdminGuard guard(AdminGuardConfig{"0.0.0.0", true});
    EXPECT_TRUE(guard.IsClientAllowed("127.0.0.1"));
    EXPECT_TRUE(guard.IsClientAllowed("192.168.1.10"));
    EXPECT_TRUE(guard.IsClientAllowed("8.8.8.8"));
}

// ---- 403 denial body (GEN-Agent 4-field schema) ----
// The handler returns this body for a non-loopback client (E2E-V3E06-2/5). A
// loopback test client always reports 127.0.0.1, so the denial path is verified
// directly on the shared body builder + the IsClientAllowed policy above.

TEST(AdminGuardDeniedBodyTest, MatchesAgentFriendlySchema) {
    json body = BuildAdminDeniedBody("192.168.1.10");
    EXPECT_EQ(body["error"]["code"], "CX_ERR_ADMIN_LOOPBACK_REQUIRED");
    EXPECT_EQ(body["error"]["retryable"], false);
    EXPECT_EQ(body["error"]["category"], "auth");
    EXPECT_EQ(body["error"]["structured_data"]["client_ip"], "192.168.1.10");
    EXPECT_EQ(body["error"]["structured_data"]["required"], "loopback");
}

// ---- pre-routing handler behavior (E2E-V3E06 matrix, design sec 13) ----

class AdminGuardServerTest : public ::testing::Test {
protected:
    void StartWith(AdminGuardConfig config) {
        port_ = 19300 + (getpid() % 400);
        // Admin + business routes so we can exercise both path classes.
        svr_.Get("/api/v1/admin/ping",
                 [](const httplib::Request&, httplib::Response& res) {
                     res.status = 200;
                     res.set_content("{\"ok\":true}", "application/json");
                 });
        svr_.Get("/api/v1/query",
                 [](const httplib::Request&, httplib::Response& res) {
                     res.status = 200;
                     res.set_content("{\"ok\":true}", "application/json");
                 });
        InstallAdminGuard(svr_, config);
        thread_ = std::thread([this] { svr_.listen("0.0.0.0", port_); });
        // httplib loopback connections report 127.0.0.1 as remote_addr, so these
        // exercise the loopback-allowed path; non-loopback denial is covered by
        // the IsClientAllowed unit tests above.
        for (int i = 0; i < 50 && !svr_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void TearDown() override {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }

    httplib::Server svr_;
    std::thread thread_;
    int port_ = 0;
};

TEST_F(AdminGuardServerTest, BusinessPathAlwaysReaches) {
    // E2E-V3E06-6: business path is never gated by AdminGuard.
    StartWith(AdminGuardConfig{});
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/query");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(AdminGuardServerTest, AdminPathFromLoopbackAllowed) {
    // E2E-V3E06-1: default config, loopback client -> 200.
    StartWith(AdminGuardConfig{});
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/admin/ping");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(AdminGuardServerTest, PublicWhitelistAdminReaches) {
    // E2E-V3E06-4: ADMIN_BIND=0.0.0.0 + ALLOW_PUBLIC_ADMIN=true -> 200 for any IP.
    StartWith(AdminGuardConfig{"0.0.0.0", true});
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/admin/ping");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

// ---- FromEnv parsing ----

TEST(AdminGuardConfigTest, FromEnvDefaults) {
    unsetenv("CORTRIX_ADMIN_BIND");
    unsetenv("CORTRIX_ALLOW_PUBLIC_ADMIN");
    AdminGuardConfig cfg = AdminGuardConfig::FromEnv();
    EXPECT_EQ(cfg.admin_bind_whitelist, "127.0.0.1");
    EXPECT_FALSE(cfg.allow_public);
}

TEST(AdminGuardConfigTest, FromEnvReadsValues) {
    setenv("CORTRIX_ADMIN_BIND", "0.0.0.0", 1);
    setenv("CORTRIX_ALLOW_PUBLIC_ADMIN", "true", 1);
    AdminGuardConfig cfg = AdminGuardConfig::FromEnv();
    EXPECT_EQ(cfg.admin_bind_whitelist, "0.0.0.0");
    EXPECT_TRUE(cfg.allow_public);
    unsetenv("CORTRIX_ADMIN_BIND");
    unsetenv("CORTRIX_ALLOW_PUBLIC_ADMIN");
}

// ---- V3-E-06 hard-fail (ValidateConfigOrAbort) ----
//
// These are death tests: ValidateConfigOrAbort() calls std::exit(1) on the
// dangerous config, and returns normally on safe configs (the test process must
// survive). They cover E2E-V3E06-3 (abort) and the safe/acknowledged paths.

TEST(AdminGuardValidateDeathTest, PublicWithoutAckAborts) {
    // E2E-V3E06-3: ADMIN_BIND=0.0.0.0 with no ALLOW_PUBLIC_ADMIN -> abort.
    setenv("CORTRIX_ADMIN_BIND", "0.0.0.0", 1);
    unsetenv("CORTRIX_ALLOW_PUBLIC_ADMIN");
    EXPECT_EXIT(AdminGuard::ValidateConfigOrAbort(),
                ::testing::ExitedWithCode(1), "RED WARNING");
    unsetenv("CORTRIX_ADMIN_BIND");
}

TEST(AdminGuardValidateTest, DefaultConfigDoesNotAbort) {
    unsetenv("CORTRIX_ADMIN_BIND");
    unsetenv("CORTRIX_ALLOW_PUBLIC_ADMIN");
    // Must return normally (no exit) for the default loopback-only config.
    AdminGuard::ValidateConfigOrAbort();
    SUCCEED();
}

TEST(AdminGuardValidateTest, AcknowledgedPublicDoesNotAbort) {
    // E2E-V3E06-4 precondition: explicit ack lets startup continue.
    setenv("CORTRIX_ADMIN_BIND", "0.0.0.0", 1);
    setenv("CORTRIX_ALLOW_PUBLIC_ADMIN", "true", 1);
    AdminGuard::ValidateConfigOrAbort();
    SUCCEED();
    unsetenv("CORTRIX_ADMIN_BIND");
    unsetenv("CORTRIX_ALLOW_PUBLIC_ADMIN");
}

}  // namespace
}  // namespace cortrix::middleware
