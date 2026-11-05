#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/middleware/admin_guard.h"
#include "cortrix/middleware/pre_routing.h"
#include "cortrix/middleware/rate_limiter.h"

namespace cortrix::middleware {
namespace {

using nlohmann::json;
using Stage = httplib::Server::HandlerWithResponse;

// A per-IP bucket of `burst` tokens that does not refill; the API-key and global
// strata are out of reach.
RateLimitRule IpRule(double burst) {
    return RateLimitRule{/*refill_per_sec=*/0.0, burst, "60s", static_cast<int64_t>(burst)};
}
RateLimitRule Unreachable() { return RateLimitRule{0.0, 1e9, "60s", 1000000000}; }

class PreRoutingServerTest : public ::testing::Test {
protected:
    // A business route and an admin route behind `stages`, on a free loopback port.
    void Start(std::vector<Stage> stages) {
        for (const char* path : {"/api/v1/health", "/api/v1/admin/ping"}) {
            svr_.Get(path, [](const httplib::Request&, httplib::Response& res) {
                res.status = 200;
                res.set_content("{\"ok\":true}", "application/json");
            });
        }
        InstallPreRouting(svr_, std::move(stages));
        port_ = svr_.bind_to_any_port("127.0.0.1");
        ASSERT_GT(port_, 0);
        thread_ = std::thread([this] { svr_.listen_after_bind(); });
        svr_.wait_until_ready();
    }

    void TearDown() override {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }

    int Get(const std::string& path, std::string* body = nullptr) {
        httplib::Client cli("127.0.0.1", port_);
        auto res = cli.Get(path);
        if (!res) return -1;
        if (body) *body = res->body;
        return res->status;
    }

    httplib::Server svr_;
    std::thread thread_;
    int port_ = 0;
};

// The server binary installs the rate limiter and AdminGuard together; with both
// installed, a client past its per-IP burst gets 429.
TEST_F(PreRoutingServerTest, RateLimitAppliesWithAdminGuardInstalled) {
    RateLimiter limiter(IpRule(3), Unreachable(), Unreachable());
    Start({RateLimitHandler(limiter), AdminGuardHandler(AdminGuardConfig{})});

    for (int i = 0; i < 3; ++i) EXPECT_EQ(Get("/api/v1/health"), 200) << "request " << i;
    std::string body;
    ASSERT_EQ(Get("/api/v1/health", &body), 429);
    const json j = json::parse(body);
    EXPECT_EQ(j["error"]["code"], "CX_ERR_RATE_LIMITED");
    EXPECT_EQ(j["error"]["structured_data"]["stratum"], "per_ip");
}

// Admin-path requests take tokens from the same per-IP bucket.
TEST_F(PreRoutingServerTest, AdminPathRequestsCountTowardTheLimit) {
    RateLimiter limiter(IpRule(2), Unreachable(), Unreachable());
    Start({RateLimitHandler(limiter), AdminGuardHandler(AdminGuardConfig{})});

    EXPECT_EQ(Get("/api/v1/admin/ping"), 200);  // loopback client: AdminGuard allows it
    EXPECT_EQ(Get("/api/v1/admin/ping"), 200);
    EXPECT_EQ(Get("/api/v1/admin/ping"), 429);
    EXPECT_EQ(Get("/api/v1/health"), 429);
}

// Stages run in order, and the first one that handles the request ends it.
TEST_F(PreRoutingServerTest, FirstHandledStageEndsTheRequest) {
    std::atomic<int> first{0}, second{0}, third{0};
    Start({
        [&first](const httplib::Request&, httplib::Response&) {
            ++first;
            return httplib::Server::HandlerResponse::Unhandled;
        },
        [&second, &first](const httplib::Request&, httplib::Response& res) {
            ++second;
            res.status = first.load() == 1 ? 418 : 500;  // the first stage ran before
            return httplib::Server::HandlerResponse::Handled;
        },
        [&third](const httplib::Request&, httplib::Response&) {
            ++third;
            return httplib::Server::HandlerResponse::Unhandled;
        },
    });

    EXPECT_EQ(Get("/api/v1/health"), 418);
    EXPECT_EQ(first.load(), 1);
    EXPECT_EQ(second.load(), 1);
    EXPECT_EQ(third.load(), 0);
}

// When no stage handles the request, it reaches its route; an empty stage is skipped.
TEST_F(PreRoutingServerTest, UnhandledRequestReachesTheRoute) {
    std::atomic<int> ran{0};
    Start({
        Stage{},
        [&ran](const httplib::Request&, httplib::Response&) {
            ++ran;
            return httplib::Server::HandlerResponse::Unhandled;
        },
    });

    EXPECT_EQ(Get("/api/v1/health"), 200);
    EXPECT_EQ(ran.load(), 1);
}

// The server binary's stages: AdminGuard only when rate limiting is off, the
// rate limiter first and then AdminGuard when it is on.
TEST(ServerPreRoutingStagesTest, RateLimiterOnlyWhenEnabled) {
    EXPECT_EQ(ServerPreRoutingStages(nullptr, AdminGuardConfig{}).size(), 1u);
    RateLimiter limiter(IpRule(1), Unreachable(), Unreachable());
    EXPECT_EQ(ServerPreRoutingStages(&limiter, AdminGuardConfig{}).size(), 2u);
}

TEST_F(PreRoutingServerTest, ServerStagesWithoutLimiterDoNotRateLimit) {
    Start(ServerPreRoutingStages(nullptr, AdminGuardConfig{}));
    for (int i = 0; i < 5; ++i) EXPECT_EQ(Get("/api/v1/health"), 200) << "request " << i;
    EXPECT_EQ(Get("/api/v1/admin/ping"), 200);  // AdminGuard still runs: loopback allowed
}

TEST_F(PreRoutingServerTest, ServerStagesWithLimiterRateLimit) {
    RateLimiter limiter(IpRule(2), Unreachable(), Unreachable());
    Start(ServerPreRoutingStages(&limiter, AdminGuardConfig{}));
    EXPECT_EQ(Get("/api/v1/health"), 200);
    EXPECT_EQ(Get("/api/v1/admin/ping"), 200);
    EXPECT_EQ(Get("/api/v1/health"), 429);
}

// The per-API-key bucket follows the Bearer key when the X-API-Key header keeps
// changing (the credential WithAuth authenticates; see RequestCredential).
TEST_F(PreRoutingServerTest, ApiKeyBucketFollowsTheBearerKey) {
    RateLimiter limiter(Unreachable(), RateLimitRule{0.0, 2.0, "60s", 2}, Unreachable());
    Start({RateLimitHandler(limiter)});
    httplib::Client cli("127.0.0.1", port_);
    int last = 0;
    for (int i = 0; i < 3; ++i) {
        auto res = cli.Get("/api/v1/health",
                           httplib::Headers{{"Authorization", "Bearer bearer-key"},
                                            {"X-API-Key", "rotating-" + std::to_string(i)}});
        ASSERT_TRUE(res);
        last = res->status;
        if (i < 2) EXPECT_EQ(last, 200) << "request " << i;
    }
    EXPECT_EQ(last, 429);
}

}  // namespace
}  // namespace cortrix::middleware
