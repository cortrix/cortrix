#include <gtest/gtest.h>
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/query_request.h"

namespace cortrix {
namespace {

RouteResult MakeOk(const std::string& name) {
    RouteResult r;
    r.route_name = name;
    r.status = RouteStatus::kSuccess;
    return r;
}

RouteResult MakeFailed(const std::string& name) {
    RouteResult r;
    r.route_name = name;
    r.status = RouteStatus::kError;
    r.error_message = "test error";
    return r;
}

RouteResult MakeTimeout(const std::string& name) {
    RouteResult r;
    r.route_name = name;
    r.status = RouteStatus::kTimeout;
    r.error_message = "timeout";
    return r;
}

RouteResult MakeSkipped(const std::string& name) {
    RouteResult r;
    r.route_name = name;
    r.status = RouteStatus::kSkipped;
    return r;
}

SearchConfig AllEnabled() {
    return SearchConfig{true, true, true};
}

SearchConfig VectorBm25Only() {
    return SearchConfig{true, true, false};
}

class DegradationManagerTest : public ::testing::Test {
protected:
    DegradationManager mgr_;
};

// --- L0: Full function ---

TEST_F(DegradationManagerTest, L0_AllRoutesOk) {
    auto info = mgr_.Evaluate(MakeOk("vector"), MakeOk("bm25"), MakeOk("sql"), AllEnabled());
    EXPECT_EQ(info.level, DegradationLevel::kL0);
    EXPECT_FALSE(info.degraded);
    EXPECT_TRUE(info.degraded_routes.empty());
    EXPECT_EQ(info.active_routes.size(), 3u);
}

TEST_F(DegradationManagerTest, L0_VectorBm25Ok_SqlDisabled) {
    auto info = mgr_.Evaluate(MakeOk("vector"), MakeOk("bm25"), MakeSkipped("sql"), VectorBm25Only());
    EXPECT_EQ(info.level, DegradationLevel::kL0);
    EXPECT_FALSE(info.degraded);
    EXPECT_EQ(info.active_routes.size(), 2u);
}

TEST_F(DegradationManagerTest, L0_NoRoutesEnabled) {
    SearchConfig config{false, false, false};
    auto info = mgr_.Evaluate(MakeSkipped("vector"), MakeSkipped("bm25"), MakeSkipped("sql"), config);
    EXPECT_EQ(info.level, DegradationLevel::kL0);
    EXPECT_FALSE(info.degraded);
}

// --- L1: No SQL ---

TEST_F(DegradationManagerTest, L1_SqlFailed_OthersOk) {
    auto info = mgr_.Evaluate(MakeOk("vector"), MakeOk("bm25"), MakeFailed("sql"), AllEnabled());
    EXPECT_EQ(info.level, DegradationLevel::kL1);
    EXPECT_TRUE(info.degraded);
    ASSERT_EQ(info.degraded_routes.size(), 1u);
    EXPECT_EQ(info.degraded_routes[0], "sql");
    EXPECT_EQ(info.active_routes.size(), 2u);
}

TEST_F(DegradationManagerTest, L1_SqlTimeout_OthersOk) {
    auto info = mgr_.Evaluate(MakeOk("vector"), MakeOk("bm25"), MakeTimeout("sql"), AllEnabled());
    EXPECT_EQ(info.level, DegradationLevel::kL1);
    EXPECT_TRUE(info.degraded);
}

// --- L1 edge case: disabled routes should not mask failures ---

TEST_F(DegradationManagerTest, L1_VectorDisabled_Bm25Ok_SqlFailed) {
    // vector disabled (user choice), bm25 ok, sql failed
    // All enabled search routes are healthy, only SQL is down -> L1
    SearchConfig config{false, true, true};
    auto info = mgr_.Evaluate(MakeSkipped("vector"), MakeOk("bm25"), MakeFailed("sql"), config);
    EXPECT_EQ(info.level, DegradationLevel::kL1);
    EXPECT_TRUE(info.degraded);
    ASSERT_EQ(info.active_routes.size(), 1u);
    EXPECT_EQ(info.active_routes[0], "bm25");
}

TEST_F(DegradationManagerTest, L1_Bm25Disabled_VectorOk_SqlFailed) {
    // bm25 disabled (user choice), vector ok, sql failed
    // All enabled search routes are healthy, only SQL is down -> L1
    SearchConfig config{true, false, true};
    auto info = mgr_.Evaluate(MakeOk("vector"), MakeSkipped("bm25"), MakeFailed("sql"), config);
    EXPECT_EQ(info.level, DegradationLevel::kL1);
    EXPECT_TRUE(info.degraded);
    ASSERT_EQ(info.active_routes.size(), 1u);
    EXPECT_EQ(info.active_routes[0], "vector");
}

TEST_F(DegradationManagerTest, L2_VectorFailed_Bm25Ok_SqlFailed) {
    // vector enabled but FAILED, bm25 ok, sql failed
    // Old buggy code: !vector_failed(false) -> short-circuits to L2 anyway (correct by accident)
    // New code: vector_enabled && !vector_ok -> sql_failed branch skipped -> falls to L2
    // This verifies the fix doesn't break: search route failed + SQL failed = L2
    auto info = mgr_.Evaluate(MakeFailed("vector"), MakeOk("bm25"), MakeFailed("sql"), AllEnabled());
    EXPECT_EQ(info.level, DegradationLevel::kL2);
    EXPECT_TRUE(info.degraded);
    EXPECT_EQ(info.degraded_routes.size(), 2u);
    ASSERT_EQ(info.active_routes.size(), 1u);
    EXPECT_EQ(info.active_routes[0], "bm25");
}

TEST_F(DegradationManagerTest, L3_BothSearchDisabled_SqlFailed) {
    // Both search routes disabled, only SQL enabled and failed -> L3
    // Old buggy code: !vector_failed(true) && !bm25_failed(true) && sql_failed(true)
    // -> L1! But no search routes are active, so this is clearly L3.
    SearchConfig config{false, false, true};
    auto info = mgr_.Evaluate(MakeSkipped("vector"), MakeSkipped("bm25"), MakeFailed("sql"), config);
    EXPECT_EQ(info.level, DegradationLevel::kL3);
    EXPECT_TRUE(info.degraded);
    EXPECT_TRUE(info.active_routes.empty());
}

// --- Fallback chain tests: vector+FTS -> FTS-only -> error ---

TEST_F(DegradationManagerTest, FallbackChain_FullFunction) {
    // Both vector and FTS work -> L0 (full function)
    auto info = mgr_.Evaluate(MakeOk("vector"), MakeOk("bm25"), MakeSkipped("sql"), VectorBm25Only());
    EXPECT_EQ(info.level, DegradationLevel::kL0);
    EXPECT_FALSE(info.degraded);
    EXPECT_EQ(info.active_routes.size(), 2u);
}

TEST_F(DegradationManagerTest, FallbackChain_VectorError_FTSOnly) {
    // Vector returns error -> gracefully fall back to FTS-only (L2)
    auto info = mgr_.Evaluate(MakeFailed("vector"), MakeOk("bm25"), MakeSkipped("sql"), VectorBm25Only());
    EXPECT_EQ(info.level, DegradationLevel::kL2);
    EXPECT_TRUE(info.degraded);
    ASSERT_EQ(info.active_routes.size(), 1u);
    EXPECT_EQ(info.active_routes[0], "bm25");
    ASSERT_EQ(info.degraded_routes.size(), 1u);
    EXPECT_EQ(info.degraded_routes[0], "vector");
}

TEST_F(DegradationManagerTest, FallbackChain_VectorTimeout_FTSOnly) {
    // Vector times out -> gracefully fall back to FTS-only (L2)
    auto info = mgr_.Evaluate(MakeTimeout("vector"), MakeOk("bm25"), MakeSkipped("sql"), VectorBm25Only());
    EXPECT_EQ(info.level, DegradationLevel::kL2);
    EXPECT_TRUE(info.degraded);
    ASSERT_EQ(info.active_routes.size(), 1u);
    EXPECT_EQ(info.active_routes[0], "bm25");
}

TEST_F(DegradationManagerTest, FallbackChain_BothFail_Error) {
    // Both vector and FTS fail -> L3 (error to caller)
    auto info = mgr_.Evaluate(MakeFailed("vector"), MakeFailed("bm25"), MakeSkipped("sql"), VectorBm25Only());
    EXPECT_EQ(info.level, DegradationLevel::kL3);
    EXPECT_TRUE(info.degraded);
    EXPECT_TRUE(info.active_routes.empty());
    EXPECT_EQ(info.degraded_routes.size(), 2u);
}

TEST_F(DegradationManagerTest, FallbackChain_FTSError_VectorOnly) {
    // FTS returns error, vector ok -> L2 (vector-only)
    auto info = mgr_.Evaluate(MakeOk("vector"), MakeFailed("bm25"), MakeSkipped("sql"), VectorBm25Only());
    EXPECT_EQ(info.level, DegradationLevel::kL2);
    EXPECT_TRUE(info.degraded);
    ASSERT_EQ(info.active_routes.size(), 1u);
    EXPECT_EQ(info.active_routes[0], "vector");
}

// --- L2: Single route ---

TEST_F(DegradationManagerTest, L2_VectorFailed_Bm25Ok) {
    auto info = mgr_.Evaluate(MakeFailed("vector"), MakeOk("bm25"), MakeSkipped("sql"), VectorBm25Only());
    EXPECT_EQ(info.level, DegradationLevel::kL2);
    EXPECT_TRUE(info.degraded);
    ASSERT_EQ(info.degraded_routes.size(), 1u);
    EXPECT_EQ(info.degraded_routes[0], "vector");
    ASSERT_EQ(info.active_routes.size(), 1u);
    EXPECT_EQ(info.active_routes[0], "bm25");
}

TEST_F(DegradationManagerTest, L2_Bm25Timeout_VectorOk) {
    auto info = mgr_.Evaluate(MakeOk("vector"), MakeTimeout("bm25"), MakeSkipped("sql"), VectorBm25Only());
    EXPECT_EQ(info.level, DegradationLevel::kL2);
    EXPECT_TRUE(info.degraded);
    ASSERT_EQ(info.active_routes.size(), 1u);
    EXPECT_EQ(info.active_routes[0], "vector");
}

TEST_F(DegradationManagerTest, L2_VectorAndSqlFailed_Bm25Ok) {
    auto info = mgr_.Evaluate(MakeFailed("vector"), MakeOk("bm25"), MakeFailed("sql"), AllEnabled());
    EXPECT_EQ(info.level, DegradationLevel::kL2);
    EXPECT_TRUE(info.degraded);
    EXPECT_EQ(info.degraded_routes.size(), 2u);
}

// --- L3: All failed ---

TEST_F(DegradationManagerTest, L3_AllFailed) {
    auto info = mgr_.Evaluate(MakeFailed("vector"), MakeFailed("bm25"), MakeFailed("sql"), AllEnabled());
    EXPECT_EQ(info.level, DegradationLevel::kL3);
    EXPECT_TRUE(info.degraded);
    EXPECT_EQ(info.degraded_routes.size(), 3u);
    EXPECT_TRUE(info.active_routes.empty());
}

TEST_F(DegradationManagerTest, L3_VectorBm25Failed_SqlDisabled) {
    auto info = mgr_.Evaluate(MakeFailed("vector"), MakeTimeout("bm25"), MakeSkipped("sql"), VectorBm25Only());
    EXPECT_EQ(info.level, DegradationLevel::kL3);
    EXPECT_TRUE(info.degraded);
}

TEST_F(DegradationManagerTest, L3_AllTimeout) {
    auto info = mgr_.Evaluate(MakeTimeout("vector"), MakeTimeout("bm25"), MakeTimeout("sql"), AllEnabled());
    EXPECT_EQ(info.level, DegradationLevel::kL3);
    EXPECT_TRUE(info.degraded);
}

}  // namespace
}  // namespace cortrix
