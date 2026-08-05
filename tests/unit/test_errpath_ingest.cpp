#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/common/status.h"
#include "cortrix/metadata/metadata_schema_provider.h"
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/spc/contextual_store.h"
#include "cortrix/spc_enricher.h"

// Error-path coverage for the ingestion-side CX_ERR_* identities that had ZERO
// test genuinely triggering them:
//   CX_ERR_DOC_ALREADY_EXISTS — per-doc submit failure re-inflated in meta.failed[]
//   CX_ERR_INVALID_CONTENT    — per-doc submit failure re-inflated in meta.failed[]
//   CX_ERR_METADATA_SCHEMA    — META block migration SQL fails (CREATE INDEX on a
//                               pre-existing, incompatible metadata_blocks table)
//   CX_ERR_CONTEXTUAL_STORE   — WriteContextualized prepare fails (target columns
//                               absent) when contextual retrieval produced output

namespace cortrix::server {
namespace {

using async::SubmitRequest;
using async::TaskInfo;

// Programmable submit seam (mirrors test_batch_submit_service.cpp's stub): a
// per-doc_id Status carrying the "CX_ERR_X: detail" token the production async task seam
// hands back, which BatchSubmitService re-inflates into the meta.failed[] item.
class StubTaskSubmitter : public ITaskSubmitter {
public:
    Result<TaskInfo> Submit(const SubmitRequest& req) override {
        auto it = fail_.find(req.doc_id);
        if (it != fail_.end()) return it->second;
        TaskInfo t;
        t.task_id = "task_" + req.doc_id;
        t.doc_id = req.doc_id;
        return t;
    }
    void FailDoc(const std::string& doc_id, Status s) { fail_[doc_id] = std::move(s); }

private:
    std::unordered_map<std::string, Status> fail_;
};

// CX_ERR_DOC_ALREADY_EXISTS: the submit seam reports a duplicate doc (async task dedup);
// BatchSubmitService::ClassifyPerDocCode reuses the originating code (§2.4.2) and
// surfaces it as the failed[] error_code with the permanent/non-retryable contract.
TEST(IngestErrPathTest, DocAlreadyExists_SurfacesInFailedItem) {
    StubTaskSubmitter stub;
    stub.FailDoc("dup",
                 Status(StatusCode::kAlreadyExists,
                        "CX_ERR_DOC_ALREADY_EXISTS: doc 'dup' already ingested"));
    BatchSubmitService svc(&stub);

    BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"dup", "content", "", ""});

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 200);  // partial-success envelope is still 200
    ASSERT_EQ(r.body["meta"]["failed"].size(), 1u);
    const auto& f = r.body["meta"]["failed"][0];
    EXPECT_EQ(f["doc_id"], "dup");
    EXPECT_EQ(f["error_code"], "CX_ERR_DOC_ALREADY_EXISTS");
    EXPECT_EQ(f["category"], "permanent");  // §2.4.2 reuse-set row = permanent
    EXPECT_EQ(f["retryable"], false);
    EXPECT_TRUE(f["retry_after_ms"].is_null());
    EXPECT_EQ(f["message"], "doc 'dup' already ingested");  // detail after the token
}

// CX_ERR_INVALID_CONTENT: the submit seam rejects unparseable content;
// re-inflated as a permanent/non-retryable per-doc failure.
TEST(IngestErrPathTest, InvalidContent_SurfacesInFailedItem) {
    StubTaskSubmitter stub;
    stub.FailDoc("bad",
                 Status(StatusCode::kInvalidArgument,
                        "CX_ERR_INVALID_CONTENT: payload is not parseable text"));
    BatchSubmitService svc(&stub);

    BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"ok", "good", "", ""});
    req.documents.push_back({"bad", "\xff\xfe junk", "", ""});

    auto r = svc.Submit(req);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["results"].size(), 1u);            // "ok" submitted
    ASSERT_EQ(r.body["meta"]["failed"].size(), 1u);     // "bad" failed
    const auto& f = r.body["meta"]["failed"][0];
    EXPECT_EQ(f["doc_id"], "bad");
    EXPECT_EQ(f["error_code"], "CX_ERR_INVALID_CONTENT");
    EXPECT_EQ(f["category"], "permanent");
    EXPECT_EQ(f["retryable"], false);
    // coverage_ratio reflects the partial success (1/2).
    EXPECT_DOUBLE_EQ(r.body["meta"]["coverage_ratio"].get<double>(), 0.5);
}

}  // namespace
}  // namespace cortrix::server

namespace cortrix::metadata {
namespace {

// CX_ERR_METADATA_SCHEMA: the META block 0→1 migration runs CREATE INDEX ... ON
// metadata_blocks(namespace_id, created_at). Pre-create a metadata_blocks table
// that lacks namespace_id so CREATE TABLE IF NOT EXISTS is a no-op but the index
// creation fails → sqlite3_exec returns non-OK → the Internal CX_ERR_METADATA_SCHEMA
// status fires (distinct from the version-mismatch path the existing suite covers).
TEST(MetadataSchemaErrPathTest, MigrationSqlFailure_SurfacesCxCode) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    // Incompatible pre-existing table: no namespace_id column.
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE metadata_blocks (block_id TEXT PRIMARY KEY, doc_id TEXT)",
        nullptr, nullptr, nullptr), SQLITE_OK);

    MetadataSchemaProvider p;
    Status s = p.Migrate(db, 0, kMetadataSchemaVersion);  // 0 → 1 full creation path

    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("CX_ERR_METADATA_SCHEMA"), std::string::npos)
        << "actual: " << s.message();
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::metadata

namespace cortrix::spc {
namespace {

// CX_ERR_CONTEXTUAL_STORE: WriteContextualized UPDATEs blocks.contextualized_* .
// On a blocks table WITHOUT those columns (ContextualSchemaProvider migration not run),
// the prepare step fails and SqlErr() surfaces the CX_ERR_CONTEXTUAL_STORE token —
// but ONLY when contextual retrieval actually produced output (status != 0), so the no-op guard is
// passed first.
TEST(ContextualStoreErrPathTest, PrepareFailureWhenColumnsMissing_SurfacesCxCode) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    // blocks table missing the contextualized_text / _embedding / _status columns.
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY, doc_id TEXT)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "INSERT INTO blocks(block_id, doc_id) VALUES(7, 'd1')",
        nullptr, nullptr, nullptr), SQLITE_OK);

    EnrichResult r;
    r.contextualized_text = "context: txt";  // Contextual retrieval ran → past the no-op guard
    r.contextualized_status = 1;             // generated
    Status s = WriteContextualized(db, 7, r);

    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("CX_ERR_CONTEXTUAL_STORE"), std::string::npos)
        << "actual: " << s.message();
    sqlite3_close(db);
}

// Control: the no-op guard means a no-F35-output result never reaches the failing
// UPDATE — it returns Ok even on the column-less table (proves the trigger above
// is the genuine prepare-failure path, not just any missing-column access).
TEST(ContextualStoreErrPathTest, NoContextualOutputIsOkEvenWithoutColumns) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY, doc_id TEXT)",
        nullptr, nullptr, nullptr), SQLITE_OK);

    EnrichResult r;  // status 0, no text/embedding → contextual retrieval never ran
    EXPECT_TRUE(WriteContextualized(db, 7, r).ok());
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::spc
