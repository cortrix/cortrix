#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "cortrix/common/executor_engine.h"
#include "cortrix/query/cross_ns_query_handler.h"
#include "mock_permission_service.h"
#include "mock_reranker.h"
#include "mock_response_builder.h"
#include "mock_scatter_executor.h"

// S4.1 coverage: POST /api/v1/query handler logic — JSON parse, B' MVP `namespace`
// deprecation (CX_ERR_DEPRECATED_FIELD), validation, dispatch + error serialization.
namespace cortrix::query {
namespace {

using ::testing::_;
using ::testing::Return;
using reranker::MockReranker;

AuthContext Auth() {
    AuthContext a;
    a.user_id = "u1";
    a.tenant_id = "t1";
    return a;
}

// Standalone harness wiring a real ScatterGather over mocks (the handler is the SUT).
struct Harness {
    MockIScatterExecutor executor;
    ExecutorEngine engine{2, 100};
    MockReranker reranker;
    MockPermissionService perm;
    ScatterGather scatter{&executor, &engine, &reranker, &perm};
    CrossNsQueryHandler handler{&scatter};

    explicit Harness(std::vector<std::string> authorized) : perm(std::move(authorized)) {}
};

// --- ParseRequest unit cases ---

TEST(CrossNsQueryHandlerParseTest, ParsesFullRequest) {
    auto body = nlohmann::json{{"query", "hello"},
                               {"namespaces", {"ns_a", "ns_b"}},
                               {"top_k", 5},
                               {"rerank", false},
                               {"search_config",
                                {{"enable_vector", true},
                                 {"enable_bm25", false},
                                 {"enable_sparse", false}}},
                               {"filter", {{"block_type", "FILE"}}}};
    QueryRequest req;
    Status s = CrossNsQueryHandler::ParseRequest(body, &req);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(req.query, "hello");
    EXPECT_EQ(req.namespaces, (std::vector<std::string>{"ns_a", "ns_b"}));
    EXPECT_EQ(req.top_k, 5);
    EXPECT_FALSE(req.rerank);
    EXPECT_TRUE(req.search_config.enable_vector);
    EXPECT_FALSE(req.search_config.enable_bm25);
    EXPECT_FALSE(req.search_config.enable_sparse);
    EXPECT_EQ(req.filter.at("block_type"), "FILE");
}

TEST(CrossNsQueryHandlerParseTest, DefaultsTopKAndRerank) {
    auto body = nlohmann::json{{"query", "q"}, {"namespaces", {"ns_a"}}};
    QueryRequest req;
    ASSERT_TRUE(CrossNsQueryHandler::ParseRequest(body, &req).ok());
    EXPECT_EQ(req.top_k, 10);
    EXPECT_TRUE(req.rerank);
}

// B' Issue 4.1: the deprecated MVP single `namespace` field → CX_ERR_DEPRECATED_FIELD.
TEST(CrossNsQueryHandlerParseTest, DeprecatedNamespaceFieldDetected) {
    auto body = nlohmann::json{{"query", "q"}, {"namespace", "ns_a"}};
    QueryRequest req;
    Status s = CrossNsQueryHandler::ParseRequest(body, &req);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message().rfind(kCxErrDeprecatedField, 0), 0u);  // tokened prefix
}

// Deprecated field is detected even when the new array is ALSO present.
TEST(CrossNsQueryHandlerParseTest, DeprecatedFieldWinsOverArray) {
    auto body = nlohmann::json{
        {"query", "q"}, {"namespace", "old"}, {"namespaces", {"new"}}};
    QueryRequest req;
    Status s = CrossNsQueryHandler::ParseRequest(body, &req);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message().rfind(kCxErrDeprecatedField, 0), 0u);
}

TEST(CrossNsQueryHandlerParseTest, RejectsMissingQuery) {
    auto body = nlohmann::json{{"namespaces", {"ns_a"}}};
    QueryRequest req;
    EXPECT_FALSE(CrossNsQueryHandler::ParseRequest(body, &req).ok());
}

TEST(CrossNsQueryHandlerParseTest, RejectsMissingNamespaces) {
    auto body = nlohmann::json{{"query", "q"}};
    QueryRequest req;
    EXPECT_FALSE(CrossNsQueryHandler::ParseRequest(body, &req).ok());
}

TEST(CrossNsQueryHandlerParseTest, RejectsEmptyNamespacesArray) {
    auto body = nlohmann::json{{"query", "q"}, {"namespaces", nlohmann::json::array()}};
    QueryRequest req;
    EXPECT_FALSE(CrossNsQueryHandler::ParseRequest(body, &req).ok());
}

TEST(CrossNsQueryHandlerParseTest, RejectsNonObjectBody) {
    QueryRequest req;
    EXPECT_FALSE(CrossNsQueryHandler::ParseRequest(nlohmann::json::array(), &req).ok());
}

// --- Handle() end-to-end (status + body) ---

// Deprecated field → 400 + the §2.4 CX_ERR_DEPRECATED_FIELD body (no scatter call).
TEST(CrossNsQueryHandlerTest, DeprecatedFieldReturns400Body) {
    Harness h({"ns_a"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, _, _)).Times(0);
    auto body = nlohmann::json{{"query", "q"}, {"namespace", "ns_a"}};
    HandlerResult r = h.handler.Handle(body, Auth());

    EXPECT_EQ(r.status, 400);
    ASSERT_TRUE(r.body.contains("error"));
    const auto& err = r.body["error"];
    EXPECT_EQ(err["code"], "CX_ERR_DEPRECATED_FIELD");
    EXPECT_FALSE(err["retryable"].get<bool>());
    EXPECT_EQ(err["category"], "permanent");
    EXPECT_EQ(err["structured_data"]["deprecated_field"], "namespace");
    EXPECT_EQ(err["structured_data"]["use_instead"], "namespaces");
}

// Malformed request → 400 bad-request body.
TEST(CrossNsQueryHandlerTest, MalformedReturns400) {
    Harness h({"ns_a"});
    HandlerResult r = h.handler.Handle(nlohmann::json{{"namespaces", {"ns_a"}}}, Auth());
    EXPECT_EQ(r.status, 400);
    ASSERT_TRUE(r.body.contains("error"));
}

// Happy path → 200 + a §2.5 body (results + meta).
TEST(CrossNsQueryHandlerTest, ValidRequestReturns200WithResults) {
    Harness h({"ns_a"});
    EXPECT_CALL(h.executor, ExecuteForNamespace(_, "ns_a", _))
        .WillOnce(Return(ScatterMockResponseBuilder::Normal("ns_a", 2)));
    auto body = nlohmann::json{{"query", "q"}, {"namespaces", {"ns_a"}}, {"top_k", 5}};
    HandlerResult r = h.handler.Handle(body, Auth());

    EXPECT_EQ(r.status, 200);
    ASSERT_TRUE(r.body.contains("results"));
    ASSERT_TRUE(r.body.contains("meta"));
    EXPECT_EQ(r.body["results"].size(), 2u);
    EXPECT_EQ(r.body["meta"]["namespaces_succeeded"].size(), 1u);
}

// Unauthorized NS → CrossNsException caught → 403 + CX_ERR_NS_UNAUTHORIZED body.
TEST(CrossNsQueryHandlerTest, UnauthorizedReturns403) {
    Harness h({"ns_a"});  // ns_secret not authorized
    auto body = nlohmann::json{{"query", "q"}, {"namespaces", {"ns_a", "ns_secret"}}};
    HandlerResult r = h.handler.Handle(body, Auth());

    EXPECT_EQ(r.status, 403);
    ASSERT_TRUE(r.body.contains("error"));
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_NS_UNAUTHORIZED");
    EXPECT_EQ(r.body["error"]["category"], "auth");
    // anti-enumeration + GEN-Agent #5: the unauthorized list is in structured_data.
    EXPECT_TRUE(r.body["error"]["structured_data"].contains("unauthorized_namespaces"));
}

// Unauthenticated → 401 + CX_ERR_AUTH_INVALID_CREDENTIALS.
TEST(CrossNsQueryHandlerTest, UnauthenticatedReturns401) {
    Harness h({"ns_a"});
    AuthContext anon;  // empty user_id
    auto body = nlohmann::json{{"query", "q"}, {"namespaces", {"ns_a"}}};
    HandlerResult r = h.handler.Handle(body, anon);
    EXPECT_EQ(r.status, 401);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_AUTH_INVALID_CREDENTIALS");
}

}  // namespace
}  // namespace cortrix::query
