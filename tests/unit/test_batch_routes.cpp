#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/async/task_error.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/server/i_task_submitter.h"
#include "cortrix/server/routes/batch_routes.h"

// Batch-submit coverage: the POST /api/v1/documents/batch route — auth (Write+),
// JSON body parsing + request-level validation (malformed JSON / missing fields /
// bad options), and the end-to-end partial-success + batch-level error wiring
// through a real BatchSubmitService over a stub submitter. Mirrors the
// test_document_routes real-httplib-server pattern.
namespace cortrix {
namespace {

using json = nlohmann::json;
using server::BatchSubmitService;
using server::ITaskSubmitter;

class StubTaskSubmitter : public ITaskSubmitter {
public:
    Result<async::TaskInfo> Submit(const async::SubmitRequest& req) override {
        auto it = fail_.find(req.doc_id);
        if (it != fail_.end()) return it->second;
        async::TaskInfo t;
        t.task_id = "task_" + req.doc_id;
        t.doc_id = req.doc_id;
        return t;
    }
    void FailDoc(const std::string& doc_id, Status s) { fail_[doc_id] = std::move(s); }
private:
    std::unordered_map<std::string, Status> fail_;
};

class BatchRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        auth_ = std::make_unique<ApiKeyAuth>();
        write_key_ = "batch-write-key";
        read_key_ = "batch-read-key";

        ApiKeyConfig wk;
        wk.key_hash = ApiKeyAuth::HashKey(write_key_);
        wk.tenant_id = "t-write";
        wk.permissions = kPermRead | kPermWrite;

        ApiKeyConfig rk;
        rk.key_hash = ApiKeyAuth::HashKey(read_key_);
        rk.tenant_id = "t-read";
        rk.permissions = kPermRead;
        auth_->LoadKeys({wk, rk});

        submitter_ = std::make_unique<StubTaskSubmitter>();
        service_ = std::make_unique<BatchSubmitService>(submitter_.get());

        port_ = 19800 + (getpid() % 400);
        server_ = std::make_unique<httplib::Server>();
        RegisterBatchRoutes(*server_, *service_, *auth_);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    httplib::Headers WriteHeaders() { return {{"Authorization", "Bearer " + write_key_}}; }
    httplib::Headers ReadHeaders() { return {{"Authorization", "Bearer " + read_key_}}; }

    std::string BodyWith(int n_docs) {
        json b;
        b["namespace"] = "ns";
        b["documents"] = json::array();
        for (int i = 0; i < n_docs; ++i) {
            b["documents"].push_back({{"doc_id", "doc_" + std::to_string(i)},
                                      {"content", "content " + std::to_string(i)}});
        }
        return b.dump();
    }

    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<StubTaskSubmitter> submitter_;
    std::unique_ptr<BatchSubmitService> service_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_;
    std::string write_key_;
    std::string read_key_;
};

// ---- auth -----------------------------------------------------------------

TEST_F(BatchRoutesTest, NoAuthRejected) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", BodyWith(2), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(BatchRoutesTest, ReadOnlyKeyForbidden) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", ReadHeaders(), BodyWith(2),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

// ---- request-level validation (standard error envelope) -------------------

TEST_F(BatchRoutesTest, MalformedJsonRejected) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), "{not json",
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(BatchRoutesTest, MissingNamespaceRejected) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "c"}}});
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(BatchRoutesTest, MissingDocumentsArrayRejected) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(BatchRoutesTest, DocItemMissingContentRejected) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "a"}}});  // no content
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// R9 robustness: a batch item with deeply-nested metadata must be rejected (422) and
// must NOT crash the server inside item["metadata"].dump() (deep-JSON overflow DoS).
TEST_F(BatchRoutesTest, DeepMetadataRejected422) {
    httplib::Client cli("127.0.0.1", port_);
    json deep = 1;
    for (int i = 0; i < 5000; ++i) deep = json{{"a", deep}};
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array(
        {{{"doc_id", "a"}, {"content", "x"}, {"metadata", deep}}});
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res) << "server crashed / no response on deep metadata";
    EXPECT_EQ(res->status, 422) << res->body;
    auto j = json::parse(res->body);
    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"]["code"], "CX_ERR_METADATA_TOO_DEEP");
}

TEST_F(BatchRoutesTest, InvalidOnDuplicateRejected) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "c"}}});
    b["options"] = {{"on_duplicate", "bogus"}};
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(BatchRoutesTest, AsyncFalseRejectedInV1) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "c"}}});
    b["options"] = {{"async", false}};
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

// ---- batch-level domain rejection (GEN-Agent envelope) --------------------

TEST_F(BatchRoutesTest, EmptyDocumentsReturnsBatchEmpty) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array();
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_BATCH_EMPTY");  // GEN-Agent wrapped envelope (R2-M8)
    EXPECT_EQ(body["error"]["category"], "permanent");
    EXPECT_EQ(body["error"]["retryable"], false);
}

TEST_F(BatchRoutesTest, SizeExceededReturns400Batch) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), BodyWith(101),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_BATCH_SIZE_EXCEEDED");
}

TEST_F(BatchRoutesTest, DuplicateDocIdReturnsBatchError) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "dup"}, {"content", "a"}},
                                  {{"doc_id", "dup"}, {"content", "b"}}});
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_BATCH_DUPLICATE_DOC_ID");
}

// ---- happy path + partial success -----------------------------------------

TEST_F(BatchRoutesTest, AllSucceedReturns200PartialSchema) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), BodyWith(3),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    ASSERT_EQ(body["results"].size(), 3u);
    EXPECT_EQ(body["results"][0]["status"], "submitted");
    EXPECT_TRUE(body["results"][0].contains("task_id"));
    EXPECT_EQ(body["meta"]["coverage_ratio"], 1.0);
    EXPECT_EQ(body["meta"]["total_submitted"], 3);
    // X-Request-Id propagated.
    EXPECT_TRUE(res->has_header("X-Request-Id"));
}

TEST_F(BatchRoutesTest, PartialSuccessReturns200WithFailedSchema) {
    submitter_->FailDoc("doc_1",
        async::TaskStatus(async::TaskErrorCode::kServiceUnavailable, "tasks_db"));
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), BodyWith(3),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;  // partial success still 200
    auto body = json::parse(res->body);
    EXPECT_EQ(body["results"].size(), 2u);
    ASSERT_EQ(body["meta"]["failed"].size(), 1u);
    const auto& f = body["meta"]["failed"][0];
    EXPECT_EQ(f["doc_id"], "doc_1");
    EXPECT_EQ(f["error_code"], "CX_ERR_SERVICE_UNAVAILABLE");
    EXPECT_EQ(f["retryable"], true);
    EXPECT_EQ(f["category"], "transient");
    EXPECT_NEAR(body["meta"]["coverage_ratio"].get<double>(), 0.6667, 0.001);
}

TEST_F(BatchRoutesTest, OnDuplicateSkipAccepted) {
    httplib::Client cli("127.0.0.1", port_);
    json b;
    b["namespace"] = "ns";
    b["documents"] = json::array({{{"doc_id", "a"}, {"content", "c"}}});
    b["options"] = {{"async", true}, {"on_duplicate", "skip"}};
    auto res = cli.Post("/api/v1/documents/batch", WriteHeaders(), b.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
}

}  // namespace
}  // namespace cortrix
