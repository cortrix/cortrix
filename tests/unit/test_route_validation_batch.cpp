// Route request-validation matrices for the F42 bulk batch-submit route
// (POST /api/v1/documents/batch). BREADTH coverage of malformed / boundary
// requests asserting the correct 4xx status + CX_ERR_* / standard-envelope body.
//
// Reaches the handler over the SAME in-process httplib server harness the
// existing test_batch_routes.cpp uses (RegisterBatchRoutes over a real
// BatchSubmitService + StubTaskSubmitter), so no live network / LLM is touched.
//
// Suite/fixture names are area-prefixed (BatchRouteValMatrix*) so they stay
// globally unique vs the existing BatchRoutesTest suite.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/server/i_task_submitter.h"
#include "cortrix/server/routes/batch_routes.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using server::BatchSubmitService;
using server::ITaskSubmitter;

// Always-accepting submitter (validation rejections fire before submission, so
// the submitter is never the source of a 4xx in these matrices).
class BatchValStubSubmitter : public ITaskSubmitter {
public:
    Result<async::TaskInfo> Submit(const async::SubmitRequest& req) override {
        async::TaskInfo t;
        t.task_id = "task_" + req.doc_id;
        t.doc_id = req.doc_id;
        return t;
    }
};

// ---------------------------------------------------------------------------
// Fixture: live in-process server, Write+ key + Read-only key.
// ---------------------------------------------------------------------------
class BatchRouteValMatrix : public ::testing::Test {
protected:
    void SetUp() override {
        auth_ = std::make_unique<ApiKeyAuth>();
        write_key_ = "brvm-write-key";
        read_key_ = "brvm-read-key";

        ApiKeyConfig wk;
        wk.key_hash = ApiKeyAuth::HashKey(write_key_);
        wk.tenant_id = "t-write";
        wk.permissions = kPermRead | kPermWrite;

        ApiKeyConfig rk;
        rk.key_hash = ApiKeyAuth::HashKey(read_key_);
        rk.tenant_id = "t-read";
        rk.permissions = kPermRead;
        auth_->LoadKeys({wk, rk});

        submitter_ = std::make_unique<BatchValStubSubmitter>();
        service_ = std::make_unique<BatchSubmitService>(submitter_.get());

        port_ = 18100 + (getpid() % 350);
        server_ = std::make_unique<httplib::Server>();
        RegisterBatchRoutes(*server_, *service_, *auth_);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    httplib::Headers Write() { return {{"Authorization", "Bearer " + write_key_}}; }
    httplib::Headers Read() { return {{"Authorization", "Bearer " + read_key_}}; }

    static std::string ValidBody(int n) {
        json b;
        b["namespace"] = "ns";
        b["documents"] = json::array();
        for (int i = 0; i < n; ++i)
            b["documents"].push_back({{"doc_id", "d" + std::to_string(i)},
                                      {"content", "c" + std::to_string(i)}});
        return b.dump();
    }

    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<BatchValStubSubmitter> submitter_;
    std::unique_ptr<BatchSubmitService> service_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_;
    std::string write_key_;
    std::string read_key_;
};

// ---- auth matrix ----------------------------------------------------------

TEST_F(BatchRouteValMatrix, NoAuthHeader401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", ValidBody(2), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(BatchRouteValMatrix, MalformedAuthScheme401) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h{{"Authorization", "Token " + write_key_}};
    auto res = cli.Post("/api/v1/documents/batch", h, ValidBody(2), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(BatchRouteValMatrix, UnknownBearerKey401) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h{{"Authorization", "Bearer not-a-real-key"}};
    auto res = cli.Post("/api/v1/documents/batch", h, ValidBody(2), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(BatchRouteValMatrix, EmptyBearerToken401) {
    httplib::Client cli("127.0.0.1", port_);
    httplib::Headers h{{"Authorization", "Bearer "}};
    auto res = cli.Post("/api/v1/documents/batch", h, ValidBody(2), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(BatchRouteValMatrix, ReadOnlyKeyForbidden403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", Read(), ValidBody(2), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// Auth precedence: a read-only key with a malformed body still gets 403 (perm
// gate fires before body parsing).
TEST_F(BatchRouteValMatrix, ReadOnlyKeyBeatsBadBody403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", Read(), "{bad", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// ---- malformed JSON matrix (TEST_P) ---------------------------------------

class BatchRouteValMalformedJson
    : public BatchRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(BatchRouteValMalformedJson, Returns400InvalidArgument) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", Write(), GetParam(), "application/json");
    ASSERT_TRUE(res) << "no response for body: " << GetParam();
    EXPECT_EQ(res->status, 400) << GetParam();
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT") << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Bodies, BatchRouteValMalformedJson,
    ::testing::Values(
        std::string("{not json"),
        std::string("}{"),
        std::string("{\"namespace\": }"),
        std::string("{\"documents\": [,]}"),
        std::string("[1,2,3"),
        std::string("\"just a string\""),
        std::string("null"),                 // valid JSON but not an object
        std::string("12345"),                // bare number
        std::string("true"),                 // bare bool
        std::string("false"),                // bare bool
        std::string("3.14159"),              // bare float
        std::string("{\"a\":\"b\",}"),       // trailing comma
        std::string("{'single':'quotes'}"),  // single quotes are not JSON
        std::string("{\"namespace\":\"ns\""),  // unterminated object
        std::string("{\"documents\":[{\"doc_id\":\"a\""),  // unterminated nested
        std::string("[]"),                   // valid JSON array, not an object
        std::string("\"\""),                 // empty JSON string
        std::string("{,}"),                  // leading comma
        std::string("undefined"),            // not JSON
        std::string("NaN"),                  // not JSON
        std::string("{\"k\": 'v'}"),         // single-quote value
        std::string("")));                   // empty body

// ---- missing / empty required-field matrix --------------------------------

TEST_F(BatchRouteValMatrix, MissingNamespace400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "c"}}});
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(BatchRouteValMatrix, EmptyNamespace400) {
    // An empty "namespace" field is rejected at the request-validation layer,
    // consistent with the flat-document surface (an empty ?namespace= → 400).
    // Documents are namespace-scoped; accepting "" would silently create a junk
    // partition instead of surfacing the caller's missing-namespace mistake.
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "";
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "c"}}});
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(BatchRouteValMatrix, MissingDocumentsArray400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(BatchRouteValMatrix, DocumentsNotArray400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = "notarray";
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400);
    EXPECT_LT(res->status, 500);
}

TEST_F(BatchRouteValMatrix, DocItemMissingContent400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "a"}}});
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(BatchRouteValMatrix, NamespaceWrongType400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = 123;  // not a string
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "c"}}});
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 400);
    EXPECT_LT(res->status, 500);
}

// ---- bad options matrix ---------------------------------------------------

TEST_F(BatchRouteValMatrix, InvalidOnDuplicate400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "c"}}});
    b["options"] = {{"on_duplicate", "bogus"}};
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(BatchRouteValMatrix, AsyncFalseRejectedV1400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "c"}}});
    b["options"] = {{"async", false}};
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// ---- batch-domain boundary matrix (CX_ERR_BATCH_*) ------------------------

TEST_F(BatchRouteValMatrix, EmptyDocumentsBatchEmpty400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array();
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_BATCH_EMPTY");
    EXPECT_EQ(body["error"]["category"], "permanent");
    EXPECT_EQ(body["error"]["retryable"], false);
}

TEST_F(BatchRouteValMatrix, OverSizeBatchSizeExceeded400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", Write(), ValidBody(101), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_BATCH_SIZE_EXCEEDED");
}

TEST_F(BatchRouteValMatrix, DuplicateDocIdBatchError400) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "dup"}, {"content", "a"}},
                                  {{"doc_id", "dup"}, {"content", "b"}}});
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_BATCH_DUPLICATE_DOC_ID");
}

TEST_F(BatchRouteValMatrix, DeepMetadataTooDeep422) {
    httplib::Client cli("127.0.0.1", port_);
    json deep = 1;
    for (int i = 0; i < 5000; ++i) deep = json{{"a", deep}};
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "x"}, {"metadata", deep}}});
    auto res = cli.Post("/api/v1/documents/batch", Write(), b.dump(), "application/json");
    ASSERT_TRUE(res) << "server crashed on deep metadata";
    EXPECT_EQ(res->status, 422) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_METADATA_TOO_DEEP");
}

// ---- batch-size boundary sweep (TEST_P) -----------------------------------
// 1..100 accepted (200); 101+ rejected (400 CX_ERR_BATCH_SIZE_EXCEEDED).

struct BatchSizeCase {
    int n;
    int expect_status;
};

class BatchRouteValSizeBoundary
    : public BatchRouteValMatrix,
      public ::testing::WithParamInterface<BatchSizeCase> {};

TEST_P(BatchRouteValSizeBoundary, StatusMatchesBoundary) {
    const auto& tc = GetParam();
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", Write(), ValidBody(tc.n), "application/json");
    ASSERT_TRUE(res) << "n=" << tc.n;
    EXPECT_EQ(res->status, tc.expect_status) << "n=" << tc.n << " body=" << res->body;
    if (tc.expect_status == 400) {
        EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_BATCH_SIZE_EXCEEDED");
    }
}

INSTANTIATE_TEST_SUITE_P(
    Sizes, BatchRouteValSizeBoundary,
    ::testing::Values(BatchSizeCase{1, 200}, BatchSizeCase{2, 200},
                      BatchSizeCase{50, 200}, BatchSizeCase{99, 200},
                      BatchSizeCase{100, 200}, BatchSizeCase{101, 400},
                      BatchSizeCase{150, 400}, BatchSizeCase{200, 400}));

// ---- method-not-allowed / unknown sub-route -------------------------------
// GET on a POST-only route is not registered => generic 404, and must NOT
// clobber any business body (anti-clobber: code is NOT_FOUND, not a CX_ERR_*).

TEST_F(BatchRouteValMatrix, GetOnPostRouteIsGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/documents/batch", Write());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(BatchRouteValMatrix, UnknownSubRouteGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch/extra", Write(), ValidBody(1),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

}  // namespace
}  // namespace cortrix
