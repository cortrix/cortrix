#include <gtest/gtest.h>
#include "cortrix/query/sql_executor.h"

namespace cortrix {
namespace {

// --- SqlExecutorStub tests ---

TEST(SqlExecutorStubTest, Execute_ReturnsSkipped) {
    SqlExecutorStub stub;
    SqlResult sql_result;
    auto result = stub.Execute("SELECT * FROM users", "default", 5000000, sql_result);

    EXPECT_EQ(result.route_name, "sql");
    EXPECT_EQ(result.status, RouteStatus::kSkipped);
    EXPECT_EQ(result.latency_us, 0);
    EXPECT_TRUE(result.items.empty());
    EXPECT_FALSE(sql_result.has_result);
}

TEST(SqlExecutorStubTest, IsAvailable_ReturnsFalse) {
    SqlExecutorStub stub;
    EXPECT_FALSE(stub.IsAvailable());
}

TEST(SqlExecutorStubTest, Execute_MultipleCalls_AlwaysSkipped) {
    SqlExecutorStub stub;

    for (int i = 0; i < 3; ++i) {
        SqlResult sql_result;
        auto result = stub.Execute("query " + std::to_string(i), "ns", 1000000, sql_result);
        EXPECT_EQ(result.status, RouteStatus::kSkipped);
        EXPECT_FALSE(sql_result.has_result);
    }
}

TEST(SqlExecutorStubTest, Execute_EmptyQuery_StillSkipped) {
    SqlExecutorStub stub;
    SqlResult sql_result;
    auto result = stub.Execute("", "", 0, sql_result);
    EXPECT_EQ(result.status, RouteStatus::kSkipped);
}

// --- SqlExecutor interface (polymorphism test) ---

TEST(SqlExecutorInterfaceTest, StubThroughBasePointer) {
    std::unique_ptr<SqlExecutor> executor = std::make_unique<SqlExecutorStub>();
    EXPECT_FALSE(executor->IsAvailable());

    SqlResult sql_result;
    auto result = executor->Execute("test query", "ns", 1000000, sql_result);
    EXPECT_EQ(result.status, RouteStatus::kSkipped);
}

}  // namespace
}  // namespace cortrix
