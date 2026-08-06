#include <gtest/gtest.h>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/query/scatter_guc.h"

// S4.2 coverage: the 5 GUCs — SoT table, range clamp, IGlobalConfig load.
namespace cortrix::query {
namespace {

// The table is exactly the 5 documented GUCs with the documented ranges.
TEST(ScatterGucTest, TableMatchesSpec) {
    ASSERT_EQ(kScatterGucs.size(), 5u);
    EXPECT_STREQ(kScatterGucs[kGucExecutorWorkers].name, "executor.workers");
    EXPECT_EQ(kScatterGucs[kGucExecutorWorkers].default_value, 8);
    EXPECT_EQ(kScatterGucs[kGucExecutorWorkers].min_value, 4);
    EXPECT_EQ(kScatterGucs[kGucExecutorWorkers].max_value, 32);

    EXPECT_STREQ(kScatterGucs[kGucMaxNamespacesPerQuery].name,
                 "scatter.max_namespaces_per_query");
    EXPECT_EQ(kScatterGucs[kGucMaxNamespacesPerQuery].default_value, 100);
    EXPECT_EQ(kScatterGucs[kGucMaxNamespacesPerQuery].max_value, 1000);

    EXPECT_STREQ(kScatterGucs[kGucNsQueryTimeoutMs].name,
                 "scatter.ns_query_timeout_ms");
    EXPECT_EQ(kScatterGucs[kGucNsQueryTimeoutMs].default_value, 5000);

    EXPECT_STREQ(kScatterGucs[kGucTotalTimeoutMs].name, "scatter.total_timeout_ms");
    EXPECT_EQ(kScatterGucs[kGucTotalTimeoutMs].default_value, 30000);
}

TEST(ScatterGucTest, ClampBelowMinAndAboveMax) {
    EXPECT_EQ(ClampScatterGuc(kGucExecutorWorkers, 2), 4);    // below min → min
    EXPECT_EQ(ClampScatterGuc(kGucExecutorWorkers, 99), 32);  // above max → max
    EXPECT_EQ(ClampScatterGuc(kGucExecutorWorkers, 16), 16);  // in range → unchanged
}

// A null cfg → all defaults (keeps ScatterGather standalone-constructible).
TEST(ScatterGucTest, NullConfigYieldsDefaults) {
    ScatterConfig c = LoadScatterConfig(nullptr);
    EXPECT_EQ(c.executor_workers, 8);
    EXPECT_EQ(c.max_namespaces_per_query, 100);
    EXPECT_EQ(c.ns_query_timeout_ms, 5000);
    EXPECT_EQ(c.total_timeout_ms, 30000);
}

TEST(ScatterGucTest, LoadsValuesFromConfig) {
    InMemoryGlobalConfig cfg;
    cfg.Set("executor.workers", "16");
    cfg.Set("scatter.max_namespaces_per_query", "50");
    cfg.Set("scatter.ns_query_timeout_ms", "2000");

    ScatterConfig c = LoadScatterConfig(&cfg);
    EXPECT_EQ(c.executor_workers, 16);
    EXPECT_EQ(c.max_namespaces_per_query, 50);
    EXPECT_EQ(c.ns_query_timeout_ms, 2000);
    EXPECT_EQ(c.total_timeout_ms, 30000);  // unset → default
}

// Out-of-range config values are clamped to the range.
TEST(ScatterGucTest, OutOfRangeConfigClamped) {
    InMemoryGlobalConfig cfg;
    cfg.Set("scatter.max_namespaces_per_query", "99999");  // > 1000
    cfg.Set("scatter.total_timeout_ms", "1");              // < 5000
    ScatterConfig c = LoadScatterConfig(&cfg);
    EXPECT_EQ(c.max_namespaces_per_query, 1000);
    EXPECT_EQ(c.total_timeout_ms, 5000);
}

// Malformed value → fall back to the (in-range) default.
TEST(ScatterGucTest, MalformedValueFallsBackToDefault) {
    InMemoryGlobalConfig cfg;
    cfg.Set("executor.workers", "not-a-number");
    ScatterConfig c = LoadScatterConfig(&cfg);
    EXPECT_EQ(c.executor_workers, 8);
}

// Timeouts() maps the two timeout GUCs onto a ScatterTimeouts.
TEST(ScatterGucTest, TimeoutsMapping) {
    ScatterConfig c;
    c.ns_query_timeout_ms = 1234;
    c.total_timeout_ms = 56789;
    ScatterTimeouts t = c.Timeouts();
    EXPECT_EQ(t.ns_query_timeout_ms, 1234);
    EXPECT_EQ(t.total_timeout_ms, 56789);
}

}  // namespace
}  // namespace cortrix::query
