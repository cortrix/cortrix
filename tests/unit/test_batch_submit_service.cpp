#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "cortrix/async/task_error.h"
#include "cortrix/server/batch_submit_service.h"

// Batch-submit coverage: BatchSubmitService — the batch submit
// orchestration, exercised standalone against a programmable ITaskSubmitter stub
// (no async task worker pool / SQLite). Covers the 4 CX_ERR_BATCH_* envelope faults, the
// 100-doc + 100MB boundaries, partial-success assembly (results[] + meta with the
// 5-field GEN-Agent failed[] + coverage_ratio), and per-doc code re-inflation.
namespace cortrix::server {
namespace {

using async::SubmitRequest;
using async::TaskInfo;

// A programmable submit seam: by default every doc succeeds with a synthetic
// task_id; specific doc_ids can be told to fail with a given async task Status (the
// "CX_ERR_X: detail" message the production seam carries).
class StubTaskSubmitter : public ITaskSubmitter {
public:
    Result<TaskInfo> Submit(const SubmitRequest& req) override {
        ++calls_;
        last_requests_.push_back(req);
        auto it = fail_.find(req.doc_id);
        if (it != fail_.end()) {
            return it->second;  // error Status
        }
        TaskInfo t;
        t.task_id = "task_" + req.doc_id;
        t.doc_id = req.doc_id;
        t.content_hash = req.content_hash;
        t.status = async::task_status::kQueued;
        return t;
    }

    void FailDoc(const std::string& doc_id, Status s) { fail_[doc_id] = std::move(s); }
    int calls() const { return calls_; }
    const std::vector<SubmitRequest>& requests() const { return last_requests_; }

private:
    int calls_ = 0;
    std::unordered_map<std::string, Status> fail_;
    std::vector<SubmitRequest> last_requests_;
};

// Build a request with `n` distinct docs of `content_each` bytes.
BatchRequest MakeRequest(int n, const std::string& content_each = "hello") {
    BatchRequest req;
    req.namespace_id = "ns";
    for (int i = 0; i < n; ++i) {
        BatchDocument d;
        d.doc_id = "doc_" + std::to_string(i);
        d.content = content_each;
        req.documents.push_back(d);
    }
    return req;
}

// ---------------------------------------------------------------------------
// Envelope validation — the 4 CX_ERR_BATCH_* faults
// ---------------------------------------------------------------------------

TEST(BatchSubmitServiceTest, EmptyDocumentsRejected) {
    StubTaskSubmitter stub;
    BatchSubmitService svc(&stub);
    BatchRequest req;  // 0 documents
    req.namespace_id = "ns";

    auto env = svc.ValidateEnvelope(req);
    ASSERT_TRUE(env.has_value());
    EXPECT_EQ(env->code, BatchErrorCode::kEmpty);

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_BATCH_EMPTY");
    EXPECT_EQ(stub.calls(), 0);  // no per-doc submit on a rejected envelope
}

TEST(BatchSubmitServiceTest, SizeExceededAt101Docs) {
    StubTaskSubmitter stub;
    BatchSubmitService svc(&stub);
    auto req = MakeRequest(101);

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_BATCH_SIZE_EXCEEDED");
    EXPECT_EQ(r.body["error"]["structured_data"]["max_size"], 100);
    EXPECT_EQ(r.body["error"]["structured_data"]["actual_size"], 101);
    EXPECT_EQ(stub.calls(), 0);
}

TEST(BatchSubmitServiceTest, Exactly100DocsAccepted) {
    // 100 is the inclusive boundary — accepted ("up to 100").
    StubTaskSubmitter stub;
    BatchSubmitService svc(&stub);
    auto req = MakeRequest(100);

    EXPECT_FALSE(svc.ValidateEnvelope(req).has_value());
    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["results"].size(), 100u);
    EXPECT_EQ(r.body["meta"]["total_submitted"], 100);
    EXPECT_DOUBLE_EQ(r.body["meta"]["coverage_ratio"].get<double>(), 1.0);
    EXPECT_EQ(stub.calls(), 100);
}

TEST(BatchSubmitServiceTest, PayloadTooLargeOverTotalCap) {
    StubTaskSubmitter stub;
    // Tiny caps so the test stays fast: total 10 bytes, per-doc 10 bytes.
    BatchLimits limits;
    limits.max_documents = 100;
    limits.max_payload_bytes = 10;
    limits.max_doc_bytes = 10;
    BatchSubmitService svc(&stub, limits);

    BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"a", "12345", ""});  // 5 bytes
    req.documents.push_back({"b", "67890", ""});  // +5 = 10 (ok)
    req.documents.push_back({"c", "X", ""});      // +1 = 11 > 10 → reject

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 413);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_BATCH_PAYLOAD_TOO_LARGE");
    EXPECT_EQ(r.body["error"]["structured_data"]["max_bytes"], 10);
}

TEST(BatchSubmitServiceTest, PayloadTooLargeOverPerDocCap) {
    StubTaskSubmitter stub;
    BatchLimits limits;
    limits.max_payload_bytes = 1000;
    limits.max_doc_bytes = 4;  // one 5-byte doc exceeds the per-doc cap
    BatchSubmitService svc(&stub, limits);

    BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"a", "12345", ""});  // 5 > 4 per-doc cap

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 413);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_BATCH_PAYLOAD_TOO_LARGE");
}

TEST(BatchSubmitServiceTest, DuplicateDocIdRejectedWithAllOffenders) {
    StubTaskSubmitter stub;
    BatchSubmitService svc(&stub);
    BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"x", "a", ""});
    req.documents.push_back({"y", "b", ""});
    req.documents.push_back({"x", "c", ""});  // dup x
    req.documents.push_back({"y", "d", ""});  // dup y

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_BATCH_DUPLICATE_DOC_ID");
    auto dups = r.body["error"]["structured_data"]["duplicate_doc_ids"];
    ASSERT_TRUE(dups.is_array());
    EXPECT_EQ(dups.size(), 2u);  // both x and y reported
    EXPECT_EQ(stub.calls(), 0);
}

TEST(BatchSubmitServiceTest, EmptyTakesPriorityOverSize) {
    // A 0-doc request is BATCH_EMPTY, never BATCH_SIZE_EXCEEDED.
    StubTaskSubmitter stub;
    BatchSubmitService svc(&stub);
    BatchRequest req;
    req.namespace_id = "ns";
    auto r = svc.Submit(req);
    EXPECT_EQ(r.body["error"]["code"], "CX_ERR_BATCH_EMPTY");
}

// ---------------------------------------------------------------------------
// Partial-success assembly
// ---------------------------------------------------------------------------

TEST(BatchSubmitServiceTest, AllSucceedFullCoverage) {
    StubTaskSubmitter stub;
    BatchSubmitService svc(&stub);
    auto req = MakeRequest(3);

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 200);
    ASSERT_EQ(r.body["results"].size(), 3u);
    EXPECT_EQ(r.body["results"][0]["doc_id"], "doc_0");
    EXPECT_EQ(r.body["results"][0]["task_id"], "task_doc_0");
    EXPECT_EQ(r.body["results"][0]["status"], "submitted");
    EXPECT_EQ(r.body["meta"]["succeeded"].size(), 3u);
    EXPECT_EQ(r.body["meta"]["failed"].size(), 0u);
    EXPECT_DOUBLE_EQ(r.body["meta"]["coverage_ratio"].get<double>(), 1.0);
    EXPECT_EQ(r.body["meta"]["total_submitted"], 3);
}

TEST(BatchSubmitServiceTest, PartialSuccessCoverageRatioAndFailedSchema) {
    StubTaskSubmitter stub;
    // doc_1 fails with a quota error, doc_3 with an LLM rate-limit (transient).
    stub.FailDoc("doc_1", async::TaskStatus(async::TaskErrorCode::kServiceUnavailable, "tasks_db"));
    BatchSubmitService svc(&stub);

    BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"doc_0", "a", ""});
    req.documents.push_back({"doc_1", "b", ""});  // fails
    req.documents.push_back({"doc_2", "c", ""});
    req.documents.push_back({"doc_3", "d", ""});

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 200);  // partial success is still 200
    EXPECT_EQ(r.body["results"].size(), 3u);       // doc_0, doc_2, doc_3
    EXPECT_EQ(r.body["meta"]["succeeded"].size(), 3u);
    ASSERT_EQ(r.body["meta"]["failed"].size(), 1u);
    EXPECT_DOUBLE_EQ(r.body["meta"]["coverage_ratio"].get<double>(), 0.75);  // 3/4
    EXPECT_EQ(r.body["meta"]["total_submitted"], 4);

    // failed[] item carries the full GEN-Agent 5-field schema + doc_id.
    const auto& f = r.body["meta"]["failed"][0];
    EXPECT_EQ(f["doc_id"], "doc_1");
    EXPECT_EQ(f["error_code"], "CX_ERR_SERVICE_UNAVAILABLE");
    EXPECT_EQ(f["category"], "transient");
    EXPECT_EQ(f["retryable"], true);
    EXPECT_EQ(f["retry_after_ms"], 10000);
    EXPECT_TRUE(f.contains("structured_data"));  // present (null standalone)
    EXPECT_TRUE(f.contains("message"));
}

TEST(BatchSubmitServiceTest, PerDocReusedCodeClassification) {
    StubTaskSubmitter stub;
    // reuse set: quota (non-retryable) vs transient LLM rate-limit.
    stub.FailDoc("q", Status(StatusCode::kInternal, "CX_ERR_NS_QUOTA_EXCEEDED: ns full"));
    stub.FailDoc("r", Status(StatusCode::kInternal, "CX_ERR_TRANSIENT_LLM_RATE_LIMIT: slow down"));
    BatchSubmitService svc(&stub);

    BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"q", "a", ""});
    req.documents.push_back({"r", "b", ""});

    auto r = svc.Submit(req);
    ASSERT_EQ(r.body["meta"]["failed"].size(), 2u);

    // Order preserved (q then r).
    const auto& fq = r.body["meta"]["failed"][0];
    EXPECT_EQ(fq["doc_id"], "q");
    EXPECT_EQ(fq["error_code"], "CX_ERR_NS_QUOTA_EXCEEDED");
    EXPECT_EQ(fq["category"], "quota");
    EXPECT_EQ(fq["retryable"], false);
    EXPECT_TRUE(fq["retry_after_ms"].is_null());
    EXPECT_EQ(fq["message"], "ns full");  // detail extracted after the token

    const auto& fr = r.body["meta"]["failed"][1];
    EXPECT_EQ(fr["doc_id"], "r");
    EXPECT_EQ(fr["error_code"], "CX_ERR_TRANSIENT_LLM_RATE_LIMIT");
    EXPECT_EQ(fr["category"], "transient");
    EXPECT_EQ(fr["retryable"], true);
    EXPECT_EQ(fr["retry_after_ms"], 5000);
}

TEST(BatchSubmitServiceTest, AllFailZeroCoverage) {
    StubTaskSubmitter stub;
    stub.FailDoc("doc_0", Status(StatusCode::kInternal, "CX_ERR_DISK_FULL: out of space"));
    stub.FailDoc("doc_1", Status(StatusCode::kInternal, "CX_ERR_DISK_FULL: out of space"));
    BatchSubmitService svc(&stub);
    auto req = MakeRequest(2);

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["results"].size(), 0u);
    EXPECT_EQ(r.body["meta"]["failed"].size(), 2u);
    EXPECT_DOUBLE_EQ(r.body["meta"]["coverage_ratio"].get<double>(), 0.0);
    // DISK_FULL is permanent / non-retryable.
    EXPECT_EQ(r.body["meta"]["failed"][0]["category"], "permanent");
    EXPECT_EQ(r.body["meta"]["failed"][0]["retryable"], false);
}

TEST(BatchSubmitServiceTest, UnknownCodeFallsBackToPermanent) {
    StubTaskSubmitter stub;
    stub.FailDoc("doc_0", Status(StatusCode::kInternal, "some non-cx error"));
    BatchSubmitService svc(&stub);
    auto req = MakeRequest(1);

    auto r = svc.Submit(req);
    const auto& f = r.body["meta"]["failed"][0];
    EXPECT_EQ(f["error_code"], "CX_ERR_INTERNAL_ERROR");  // safe fallback token
    EXPECT_EQ(f["category"], "permanent");
    EXPECT_EQ(f["retryable"], false);
}

TEST(BatchSubmitServiceTest, SubmitRequestCarriesNamespaceDocIdAndContentHash) {
    StubTaskSubmitter stub;
    BatchSubmitService svc(&stub);
    BatchRequest req;
    req.namespace_id = "my-ns";
    req.documents.push_back({"doc_x", "the content", ""});

    svc.Submit(req);
    ASSERT_EQ(stub.requests().size(), 1u);
    const auto& sr = stub.requests()[0];
    EXPECT_EQ(sr.namespace_id, "my-ns");
    EXPECT_EQ(sr.doc_id, "doc_x");
    EXPECT_EQ(sr.content_hash.rfind("sha256:", 0), 0u);  // content-addressed
    EXPECT_EQ(sr.task_type, async::kTaskDocParse);
}

}  // namespace
}  // namespace cortrix::server
