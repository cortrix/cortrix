// Error-path coverage for the doc-summary worker codes, the DB import
// schema/handler codes. Each CX_ERR_* below had ZERO referencing test.
//
// Doc summary codes ride on the Status message prefix ("<CODE>: <detail>", via
// TaskFinalizer::Fail) — assert with message().find(code). F16A_SCHEMA likewise
// rides on a Status message. F16A_INTERNAL surfaces as a real envelope error.code
// (the explicit catch(std::exception&) in ImportHandler::HandleStartImport, NOT the
// generic global exception handler) — assert the JSON field.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// --- doc summary worker deps (mirror test_f41_async_worker.cpp) ---
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_type.h"
#include "cortrix/chunker/types.h"
#include "cortrix/common/block_types.h"
#include "cortrix/common/data_types.h"
#include "cortrix/doc_summary/doc_summary_metrics.h"
#include "cortrix/doc_summary/f41_async_worker.h"
#include "cortrix/id/hash.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/onnx_embedder.h"

// --- DB import deps (mirror test_import_manager.cpp / test_f16a_schema_provider.cpp) -----
#include "cortrix/import/connection_manager.h"
#include "cortrix/import/f16a_schema_provider.h"
#include "cortrix/import/import_manager.h"
#include "cortrix/import/query_executor.h"
#include "cortrix/import/spc_feed.h"
#include "cortrix/import/text_serializer.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/server/import_handler.h"

#include "mock_llm_client.h"
#include "ns_pool_test_helper.h"

// ===========================================================================
// Doc summary async-worker error paths.
// ===========================================================================
namespace cortrix::doc_summary {
namespace {

using ::testing::_;
using ::testing::NiceMock;

llm::ChatCompletionResponse OkChat(std::string content) {
    llm::ChatCompletionResponse r;
    r.content = std::move(content);
    r.model = "gpt-4o-mini";
    r.prompt_tokens = 100;
    r.completion_tokens = 60;
    return r;
}

const char* kSummaryJson = R"({
  "summary_text": "Q3 2026 financials: revenue grew twenty three percent year over year.",
  "keywords": ["revenue", "finance", "Q3"],
  "topics": ["Financial Report"],
  "one_liner": "Q3 2026 financials"
})";

class F41ErrPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        id::SetDeploymentHashKeyForTesting({0x0123456789abcdefULL, 0xfedcba9876543210ULL});
        DocSummaryMetrics::Instance().ResetForTest();

        harness_ = std::make_unique<test::NsPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("f41_errpath_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        ASSERT_TRUE(harness_->Admit("test-ns").ok());

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();  // Init'd embedder for the success/NS/WRITE paths.

        ASSERT_TRUE(mgr_.Init(":memory:").ok());
    }
    void TearDown() override { harness_.reset(); }

    std::string SeedDoc(int n_chunks) {
        resource::NamespaceFacade f(harness_->ipool(), "test-ns");
        EXPECT_TRUE(f.Acquire().ok());
        CortrixDoc doc;
        doc.source_type = "test";
        doc.source_path = "/f41/doc.txt";
        EXPECT_EQ(f.store().doc_create(doc), 0);
        BlockAssembler assembler;
        for (int i = 0; i < n_chunks; ++i) {
            chunker::ChildChunk c;
            c.child_id = "ch-" + doc.doc_id + "-" + std::to_string(i);
            c.parent_id = "par-" + doc.doc_id;
            c.doc_id = doc.doc_id;
            c.chunk_index = i;
            c.child_text = "chunk body number " + std::to_string(i);
            EmbeddingResult emb;
            CortrixBlock b = assembler.AssembleChild(c, emb, kBlockFile, 3, "");
            EXPECT_EQ(f.store().block_insert(b), 0);
        }
        return doc.doc_id;
    }

    std::shared_ptr<NiceMock<llm::MockLlmClient>> MakeLlm(const char* json) {
        auto m = std::make_shared<NiceMock<llm::MockLlmClient>>();
        ON_CALL(*m, Chat(_, _)).WillByDefault(
            [json](const std::string&, const llm::LlmCallConfig&) { return OkChat(json); });
        return m;
    }

    async::TaskInfo SummaryTask(const std::string& doc_id, const std::string& ns = "test-ns") {
        async::TaskInfo t;
        t.doc_id = doc_id;
        t.namespace_id = ns;
        t.task_type = async::kTaskDocSummary;
        return t;
    }

    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    BlockAssembler assembler_;
    async::TaskManager mgr_;
};

// CX_ERR_DOCSUMMARY_NS_ACQUIRE_FAILED — acquiring a never-admitted namespace fails before
// generation/embed/write (f41_async_worker.cpp:58).
TEST_F(F41ErrPathTest, UnknownNamespaceReturnsNsAcquireFailed) {
    F41AsyncWorker worker(harness_->ipool(), MakeLlm(kSummaryJson), DocSummaryConfig{},
                          *embedder_, assembler_, &mgr_);
    Status s = worker.ProcessTask(SummaryTask("some-doc", "no-such-ns"));
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_DOCSUMMARY_NS_ACQUIRE_FAILED"), std::string::npos)
        << "message: " << s.message();
}

// CX_ERR_DOCSUMMARY_EMBED_FAILED — embedding the summary fails (f41_async_worker.cpp:80).
// An OnnxEmbedder that was never Init()'d returns Internal("embedder not
// initialized") from Embed(), AFTER generation succeeds.
TEST_F(F41ErrPathTest, UninitializedEmbedderReturnsEmbedFailed) {
    const std::string doc_id = SeedDoc(2);
    OnnxEmbedder uninit("", 128);  // deliberately NOT Init()'d -> Embed fails
    F41AsyncWorker worker(harness_->ipool(), MakeLlm(kSummaryJson), DocSummaryConfig{},
                          uninit, assembler_, &mgr_);
    Status s = worker.ProcessTask(SummaryTask(doc_id));
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_DOCSUMMARY_EMBED_FAILED"), std::string::npos)
        << "message: " << s.message();
}

// CX_ERR_DOCSUMMARY_WRITE_FAILED — the vector AddPoints into the P-HNSW index fails
// (f41_async_worker.cpp:113). FakeIndex::set_add_should_fail makes AddPoints return
// kUnavailable; generation+embed must succeed first (Init'd embedder + chunks) so
// AddPoints is actually reached.
TEST_F(F41ErrPathTest, VectorAddFailureReturnsWriteFailed) {
    const std::string doc_id = SeedDoc(2);
    harness_->fake_index()->set_add_should_fail(true);
    F41AsyncWorker worker(harness_->ipool(), MakeLlm(kSummaryJson), DocSummaryConfig{},
                          *embedder_, assembler_, &mgr_);
    Status s = worker.ProcessTask(SummaryTask(doc_id));
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_DOCSUMMARY_WRITE_FAILED"), std::string::npos)
        << "message: " << s.message();
}

}  // namespace
}  // namespace cortrix::doc_summary

// ===========================================================================
// DB import schema + handler error paths.
// ===========================================================================
namespace cortrix::import {
namespace {

// CX_ERR_IMPORT_SCHEMA — F16aSchemaProvider::Migrate fails when one of its
// "CREATE ... IF NOT EXISTS" statements collides on object TYPE: pre-creating a
// TABLE named like one of the provider's indices makes the CREATE INDEX error
// ("there is already a table named idx_import_tasks_running"), which IF NOT EXISTS
// does not suppress on a type mismatch.
TEST(F16aSchemaErrPathTest, IndexNameCollisionReturnsSchemaError) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    char* err = nullptr;
    // A TABLE occupying the name the provider wants for an INDEX.
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE idx_import_tasks_running (x INTEGER);",
        nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);

    F16aSchemaProvider p;
    Status s = p.Migrate(db, 0, 1);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_IMPORT_SCHEMA"), std::string::npos)
        << "message: " << s.message();
    sqlite3_close(db);
}

// An IQueryExecutor whose EstimateRowCount throws a non-import std::exception. This is
// invoked inside ImportManager::StartImport (after the admin check + DSN resolve),
// so the throw propagates out of StartImport and is caught by the handler's
// catch(std::exception&) -> CX_ERR_IMPORT_INTERNAL.
class ThrowingQueryExecutor : public IQueryExecutor {
public:
    Result<int64_t> EstimateRowCount(const std::string&, const QueryRequest&,
                                     const QueryConstraints&) override {
        throw std::runtime_error("simulated DB driver fault");
    }
    Result<std::vector<DbRow>> Execute(const std::string&, const QueryRequest&,
                                       const QueryConstraints&) override {
        return std::vector<DbRow>{};
    }
};

AuthContext AdminCtx() {
    AuthContext c;
    c.tenant_id = "t1";
    c.user_id = "admin1";
    c.permissions = kPermAdmin | kPermRead | kPermWrite;
    return c;
}

// CX_ERR_IMPORT_INTERNAL — explicit catch in ImportHandler::HandleStartImport
// (import_handler.cpp:134-145), http 500, error.code in the envelope.
TEST(F16aHandlerErrPathTest, ThrowingImportStackReturnsF16aInternal) {
    auto secret = std::make_shared<InMemorySecretStore>();
    auto conn_store = std::make_shared<InMemoryConnectionStore>();
    auto conn_mgr = std::make_shared<ConnectionManager>(secret, conn_store);
    auto reg = conn_mgr->Register("crm", "postgres://u:p@db.internal:5432/crm", "t1", "admin1");
    ASSERT_TRUE(reg.ok());
    const ConnectionRefId ref = reg.value();

    auto query_exec = std::make_shared<ThrowingQueryExecutor>();
    auto serializer = std::make_shared<TextSerializer>();
    auto feeder = std::make_shared<InMemorySpcFeeder>();
    auto cleaner = std::make_shared<InMemoryBlockCleaner>();
    auto task_store = std::make_shared<InMemoryImportTaskStore>();
    auto oplog = std::shared_ptr<observability::IOperationLogger>(nullptr);

    ImportManagerConfig cfg;
    cfg.pg_host = "db.internal";
    cfg.pg_port = 5432;
    cfg.pg_db = "crm";
    auto mgr = std::make_shared<ImportManager>(conn_mgr, query_exec, serializer, feeder,
                                               cleaner, task_store, oplog, cfg);

    server::ImportHandler handler(mgr, conn_mgr);
    nlohmann::json body = {
        {"namespace", "customer_kb"},
        {"connection_ref", ref},
        {"table", "users"},
    };
    int http = 0;
    nlohmann::json out = handler.HandleStartImport(body, AdminCtx(), http);

    EXPECT_EQ(http, 500) << out.dump();
    ASSERT_TRUE(out.contains("error"));
    EXPECT_EQ(out["error"]["code"], "CX_ERR_IMPORT_INTERNAL") << out.dump();
}

}  // namespace
}  // namespace cortrix::import
