#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "cortrix/common/in_memory_global_config.h"
#include "mock_global_config.h"

namespace cortrix {
namespace {

using ::testing::_;
using ::testing::Return;

TEST(IGlobalConfigTest, PodReservedFieldDefaults) {
    InMemoryGlobalConfig cfg;
    EXPECT_EQ(cfg.operation_log_retention_days, 30);
    EXPECT_EQ(cfg.operation_log_max_rows, 100000);
    EXPECT_EQ(cfg.agent_trace_retention_days, 90);
    EXPECT_EQ(cfg.interaction_log_retention_days, 180);
    EXPECT_EQ(cfg.f13_mcp_idle_timeout_seconds, 1800);
}

TEST(InMemoryGlobalConfigTest, TypedGettersParse) {
    InMemoryGlobalConfig cfg;
    cfg.Set("enricher.endpoint", "https://api.example.com");
    cfg.Set("enricher.enabled", "true");
    cfg.Set("reranker.workers", "8");
    cfg.Set("crag.threshold", "0.75");

    EXPECT_EQ(cfg.GetString("enricher.endpoint").value(), "https://api.example.com");
    EXPECT_TRUE(cfg.GetBool("enricher.enabled").value());
    EXPECT_EQ(cfg.GetInt("reranker.workers").value(), 8);
    EXPECT_FLOAT_EQ(cfg.GetFloat("crag.threshold").value(), 0.75f);
}

TEST(InMemoryGlobalConfigTest, MissingKeyIsNotFound) {
    InMemoryGlobalConfig cfg;
    auto r = cfg.GetString("nope");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
}

TEST(InMemoryGlobalConfigTest, MalformedValueIsInvalidArgument) {
    InMemoryGlobalConfig cfg;
    cfg.Set("reranker.workers", "eight");
    auto r = cfg.GetInt("reranker.workers");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
}

TEST(InMemoryGlobalConfigTest, OnChangeFiresOnSet) {
    InMemoryGlobalConfig cfg;
    std::string changed_key;
    int calls = 0;
    cfg.OnChange([&](const std::string& key) {
        changed_key = key;
        ++calls;
    });

    cfg.Set("enricher.endpoint", "https://x");
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(changed_key, "enricher.endpoint");
}

// Downstream feature (e.g. operation log) consuming config through the interface + mock.
TEST(MockGlobalConfigTest, DownstreamReadsThroughInterface) {
    MockGlobalConfig mock;
    EXPECT_CALL(mock, GetInt("operation_log.retention_days"))
        .WillOnce(Return(Result<int>(365)));

    IGlobalConfig* cfg = &mock;
    auto r = cfg->GetInt("operation_log.retention_days");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 365);
}

}  // namespace
}  // namespace cortrix
