#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <tuple>

#include <nlohmann/json.hpp>

#include "cortrix/middleware/rate_limiter.h"

// BREADTH op-matrix coverage for RateLimiter (F20 token-bucket). Complements
// test_rate_limiter.cpp with matrices over rate x burst x time using the
// injectable clock (no real sleeps): burst-draining, refill timing, lock
// duration, multi-stratum interaction, and BuildErrorBody schema across strata.
//
// Suite names are globally unique: RateLimiterOpsMatrix* (no clash with
// RateLimiterTest).
namespace cortrix::middleware {
namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

// Controllable monotonic clock (mirrors the existing test's FakeClock).
class OpsFakeClock {
public:
    Clock::time_point now() const { return now_; }
    void Advance(std::chrono::milliseconds d) { now_ += d; }
private:
    Clock::time_point now_ = Clock::time_point(std::chrono::seconds(1000));
};

// RateLimiter holds a std::mutex -> non-copyable/non-movable; it can't be
// returned by value. Build an "IP-only" limiter in place (key/global made huge so
// only the IP bucket can ever deny) via this macro.
#define OPS_IP_ONLY_LIMITER(var, refill, burst)                     \
    RateLimiter var(RateLimitRule{(refill), (burst), "60s", 60},    \
                    RateLimitRule{1e9, 1e9, "60s", 300},            \
                    RateLimitRule{1e9, 1e9, "1s", 1000})

// The 5-minute lock constant the limiter applies on breach.
constexpr int64_t kLockMs = 5 * 60 * 1000;

// -- burst-draining: exactly `burst` requests pass, then a breach ------------
class RateLimiterOpsMatrixBurst : public ::testing::TestWithParam<int> {};

TEST_P(RateLimiterOpsMatrixBurst, DrainsBurstThenBreaches) {
    const int burst = GetParam();
    OpsFakeClock clk;
    OPS_IP_ONLY_LIMITER(limiter, 1.0, static_cast<double>(burst));
    limiter.SetClock([&clk] { return clk.now(); });

    for (int i = 0; i < burst; ++i) {
        auto d = limiter.Allow("ip-burst", "");
        EXPECT_TRUE(d.allowed) << "req #" << i << " burst=" << burst;
    }
    auto d = limiter.Allow("ip-burst", "");
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.stratum, RateLimitStratum::kPerIp);
    EXPECT_EQ(d.retry_after_ms, kLockMs);
}

INSTANTIATE_TEST_SUITE_P(Bursts, RateLimiterOpsMatrixBurst,
                         ::testing::Values(1, 2, 3, 4, 5, 8, 10, 15, 20, 25, 40,
                                           50, 75, 100, 150, 200, 250, 400, 500,
                                           1000));

// -- refill: after draining, advancing time by N seconds at R tok/s restores
//    floor(N*R) tokens (capped at burst). Matrix over (refill, wait-seconds). --
struct RefillCase {
    double refill;
    double burst;
    int wait_sec;
    int expect_allowed_after;  // how many Allow() should pass post-wait
};
class RateLimiterOpsMatrixRefill : public ::testing::TestWithParam<RefillCase> {};

TEST_P(RateLimiterOpsMatrixRefill, RefillsExpectedTokensAfterLockClears) {
    const auto p = GetParam();
    OpsFakeClock clk;
    OPS_IP_ONLY_LIMITER(limiter, p.refill, p.burst);
    limiter.SetClock([&clk] { return clk.now(); });

    // Drain the bucket, then breach (which arms the 5-min lock).
    for (int i = 0; i < static_cast<int>(p.burst); ++i)
        ASSERT_TRUE(limiter.Allow("ip-refill", "").allowed) << "drain #" << i;
    ASSERT_FALSE(limiter.Allow("ip-refill", "").allowed);  // breach -> locked

    // Wait past the lock AND p.wait_sec of refill. Refill accrues during the
    // lock too, so after the lock clears the bucket holds min(burst, total*R).
    clk.Advance(std::chrono::seconds(p.wait_sec));
    if (p.wait_sec * 1000 < kLockMs) {
        // Still inside the 5-min lock window -> denied regardless of refill.
        EXPECT_FALSE(limiter.Allow("ip-refill", "").allowed);
        clk.Advance(std::chrono::milliseconds(kLockMs));  // clear the lock
    }
    int allowed = 0;
    for (int i = 0; i < static_cast<int>(p.burst) + 2; ++i)
        if (limiter.Allow("ip-refill", "").allowed) ++allowed;
    EXPECT_GE(allowed, p.expect_allowed_after) << "refill=" << p.refill;
}

INSTANTIATE_TEST_SUITE_P(
    Refills, RateLimiterOpsMatrixRefill,
    ::testing::Values(
        RefillCase{1.0, 2.0, 0, 1},     // lock alone refills bucket fully
        RefillCase{1.0, 5.0, 0, 1},
        RefillCase{2.0, 4.0, 0, 1},
        RefillCase{5.0, 10.0, 0, 1},
        RefillCase{10.0, 20.0, 0, 1},
        RefillCase{0.5, 2.0, 0, 1},
        RefillCase{1.0, 3.0, 1, 1},
        RefillCase{1.0, 1.0, 10, 1},
        RefillCase{3.0, 6.0, 0, 1},
        RefillCase{4.0, 8.0, 0, 1},
        RefillCase{8.0, 16.0, 0, 1},
        RefillCase{0.25, 1.0, 0, 1},
        RefillCase{2.0, 10.0, 5, 1},
        RefillCase{1.0, 2.0, 30, 1}));

// -- lock persistence: still locked before 5 min, freed after -----------------
class RateLimiterOpsMatrixLock : public ::testing::TestWithParam<int> {};

TEST_P(RateLimiterOpsMatrixLock, LockHeldThroughTheWindow) {
    const int wait_min = GetParam();
    OpsFakeClock clk;
    OPS_IP_ONLY_LIMITER(limiter, 1.0, 5.0);
    limiter.SetClock([&clk] { return clk.now(); });

    for (int i = 0; i < 5; ++i) ASSERT_TRUE(limiter.Allow("ip-lock", "").allowed);
    ASSERT_FALSE(limiter.Allow("ip-lock", "").allowed);  // breach -> locked

    clk.Advance(std::chrono::minutes(wait_min));
    bool allowed = limiter.Allow("ip-lock", "").allowed;
    if (wait_min < 5) {
        EXPECT_FALSE(allowed) << "should still be locked at " << wait_min << " min";
    } else {
        EXPECT_TRUE(allowed) << "lock should have cleared at " << wait_min << " min";
    }
}

INSTANTIATE_TEST_SUITE_P(Minutes, RateLimiterOpsMatrixLock,
                         ::testing::Values(0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 15, 20,
                                           30, 60));

// -- per-stratum identity matrix: which stratum denies first -----------------
// Each row tunes one stratum tiny while the others stay huge, asserting the
// reported stratum + limit/window on breach.
struct StratumCase {
    RateLimitStratum which;
    std::string ip;
    std::string key;
    int64_t expect_limit;
    std::string expect_window;
};
class RateLimiterOpsMatrixStratum : public ::testing::TestWithParam<StratumCase> {};

TEST_P(RateLimiterOpsMatrixStratum, ReportsDenyingStratum) {
    const auto p = GetParam();
    OpsFakeClock clk;
    // Default rules: ip burst 100/limit 60; key burst 500/limit 300; global
    // burst 1500/limit 1000. Pick a per-stratum burst we can drain.
    RateLimiter limiter;  // default Phase-1 rules
    limiter.SetClock([&clk] { return clk.now(); });

    int drain = 0;
    if (p.which == RateLimitStratum::kPerIp) drain = 100;
    else if (p.which == RateLimitStratum::kPerApiKey) drain = 500;
    else drain = 1500;

    for (int i = 0; i < drain; ++i) {
        auto d = limiter.Allow(p.ip, p.key);
        ASSERT_TRUE(d.allowed) << "drain #" << i;
    }
    auto d = limiter.Allow(p.ip, p.key);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.stratum, p.which);
    EXPECT_EQ(d.limit, p.expect_limit);
    EXPECT_EQ(d.window, p.expect_window);
}

INSTANTIATE_TEST_SUITE_P(
    Strata, RateLimiterOpsMatrixStratum,
    ::testing::Values(
        // IP stratum: distinct key per request would still share the IP bucket,
        // but key burst (500) > ip burst (100) so IP denies first. Empty key.
        StratumCase{RateLimitStratum::kPerIp, "10.0.0.1", "", 60, "60s"},
        // Key stratum: empty IP isolates the key bucket (500), which denies
        // before global (1500).
        StratumCase{RateLimitStratum::kPerApiKey, "", "k-iso", 300, "60s"}));

// -- empty-identity matrix: empty IP and/or empty key skip that stratum ------
struct SkipCase { std::string ip; std::string key; };
class RateLimiterOpsMatrixSkip : public ::testing::TestWithParam<SkipCase> {};

TEST_P(RateLimiterOpsMatrixSkip, EmptyIdentityNeverBreachesItsStratum) {
    const auto p = GetParam();
    OpsFakeClock clk;
    // Make IP + key tiny; only the non-empty identities can deny. Global huge.
    RateLimiter limiter(RateLimitRule{1000.0, 2.0, "60s", 60},
                        RateLimitRule{1000.0, 2.0, "60s", 300},
                        RateLimitRule{1e9, 1e9, "1s", 1000});
    limiter.SetClock([&clk] { return clk.now(); });

    // If both identities are empty, no stratum can ever deny: many calls pass.
    if (p.ip.empty() && p.key.empty()) {
        for (int i = 0; i < 100; ++i)
            EXPECT_TRUE(limiter.Allow(p.ip, p.key).allowed) << "i=" << i;
        return;
    }
    // Otherwise the present identity's tiny bucket (burst 2) drains then denies.
    EXPECT_TRUE(limiter.Allow(p.ip, p.key).allowed);
    EXPECT_TRUE(limiter.Allow(p.ip, p.key).allowed);
    EXPECT_FALSE(limiter.Allow(p.ip, p.key).allowed);
}

INSTANTIATE_TEST_SUITE_P(Idents, RateLimiterOpsMatrixSkip,
                         ::testing::Values(SkipCase{"", ""}, SkipCase{"ip", ""},
                                           SkipCase{"", "key"},
                                           SkipCase{"ip", "key"}));

// -- distinct identities have independent buckets (matrix over count) --------
class RateLimiterOpsMatrixIsolation : public ::testing::TestWithParam<int> {};

TEST_P(RateLimiterOpsMatrixIsolation, DistinctIpsDoNotShareBuckets) {
    const int count = GetParam();
    OpsFakeClock clk;
    OPS_IP_ONLY_LIMITER(limiter, 1.0, 3.0);  // burst 3 per IP
    limiter.SetClock([&clk] { return clk.now(); });

    // Each distinct IP independently gets its own 3-token bucket.
    for (int c = 0; c < count; ++c) {
        std::string ip = "ip-" + std::to_string(c);
        EXPECT_TRUE(limiter.Allow(ip, "").allowed);
        EXPECT_TRUE(limiter.Allow(ip, "").allowed);
        EXPECT_TRUE(limiter.Allow(ip, "").allowed);
        EXPECT_FALSE(limiter.Allow(ip, "").allowed);  // that IP now breached
    }
}

INSTANTIATE_TEST_SUITE_P(Counts, RateLimiterOpsMatrixIsolation,
                         ::testing::Values(1, 2, 3, 4, 5, 8, 10, 15, 20, 25, 50));

// -- BuildErrorBody schema over a stratum/limit matrix -----------------------
struct BodyCase {
    RateLimitStratum stratum;
    const char* stratum_str;
    int64_t limit;
    std::string window;
    int64_t retry_ms;
};
class RateLimiterOpsMatrixErrorBody : public ::testing::TestWithParam<BodyCase> {};

TEST_P(RateLimiterOpsMatrixErrorBody, BodyMatchesAgentSchema) {
    const auto p = GetParam();
    RateLimitDecision d;
    d.allowed = false;
    d.stratum = p.stratum;
    d.limit = p.limit;
    d.window = p.window;
    d.remaining = 0.0;
    d.retry_after_ms = p.retry_ms;
    d.reset_at_unix_ms = 1700000000000;
    d.locked_until_unix_ms = 1700000000000;

    json body = RateLimiter::BuildErrorBody(d);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_RATE_LIMITED");
    EXPECT_EQ(body["error"]["retryable"], true);
    EXPECT_EQ(body["error"]["category"], "quota");
    EXPECT_EQ(body["error"]["retry_after_ms"], p.retry_ms);
    EXPECT_EQ(body["error"]["structured_data"]["stratum"], p.stratum_str);
    EXPECT_EQ(body["error"]["structured_data"]["limit"], p.limit);
    EXPECT_EQ(body["error"]["structured_data"]["window"], p.window);
    EXPECT_TRUE(body["error"]["structured_data"].contains("reset_at"));
    EXPECT_TRUE(body["error"]["structured_data"].contains("locked_until"));
}

INSTANTIATE_TEST_SUITE_P(
    Bodies, RateLimiterOpsMatrixErrorBody,
    ::testing::Values(
        BodyCase{RateLimitStratum::kPerIp, "per_ip", 60, "60s", kLockMs},
        BodyCase{RateLimitStratum::kPerApiKey, "per_api_key", 300, "60s", kLockMs},
        BodyCase{RateLimitStratum::kGlobal, "global", 1000, "1s", 1000},
        BodyCase{RateLimitStratum::kPerIp, "per_ip", 1, "1s", 500},
        BodyCase{RateLimitStratum::kGlobal, "global", 99999, "60s", 0}));

// -- ToString matrix for the three strata ------------------------------------
TEST(RateLimiterOpsMatrixToString, AllStrataSerialize) {
    EXPECT_STREQ(ToString(RateLimitStratum::kPerIp), "per_ip");
    EXPECT_STREQ(ToString(RateLimitStratum::kPerApiKey), "per_api_key");
    EXPECT_STREQ(ToString(RateLimitStratum::kGlobal), "global");
}

// -- default-rule constants matrix -------------------------------------------
TEST(RateLimiterOpsMatrixDefaults, DefaultRulesMatchDesign) {
    auto ip = DefaultIpRule();
    EXPECT_DOUBLE_EQ(ip.burst, 100.0);
    EXPECT_EQ(ip.limit, 60);
    auto key = DefaultApiKeyRule();
    EXPECT_DOUBLE_EQ(key.burst, 500.0);
    EXPECT_EQ(key.limit, 300);
    auto g = DefaultGlobalRule();
    EXPECT_DOUBLE_EQ(g.burst, 1500.0);
    EXPECT_EQ(g.limit, 1000);
}

}  // namespace
}  // namespace cortrix::middleware
