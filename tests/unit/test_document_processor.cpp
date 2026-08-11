#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

#include "cortrix/async/document_processor.h"
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_errors.h"
#include "cortrix/spc/parser_factory.h"
#include "parser_stub.h"  // cortrix::spc::test::StubParser / MakeOnePageDoc
#include "cortrix/spc/spc_task.h"
#include "mock_spc_manager.h"
#include "test_name_util.h"

// S2 coverage: DocumentProcessor — F06 integration via on_page_progress (per-page
// persistence + Issue 3.1 cancel checkpoint + Issue 5 error mapping). The injected
// StubParser drives on_page_progress to simulate F06's per-page streaming, so the
// processor's callback path is exercised standalone (no Python).
namespace cortrix::async {
namespace {

using spc::ParsedDoc;
using spc::ParserErrorCode;
using spc::ParserOptions;
using spc::test::MakeOnePageDoc;
using spc::test::StubParser;

// A StubParser that, when ParseDocument runs, fires opts.on_page_progress for
// pages 1..total_pages (success unless the page is in fail_pages), then returns
// `result`. This mimics the real F06 per-page streaming the factory would do.
std::unique_ptr<StubParser> MakeDrivingStub(int total_pages, ParsedDoc result,
                                            std::vector<int> fail_pages = {}) {
    auto stub = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
    stub->SetResult(std::move(result));
    stub->SetOnParse([total_pages, fail_pages](const std::string&,
                                               const ParserOptions& opts) {
        if (!opts.on_page_progress) return;
        for (int p = 1; p <= total_pages; ++p) {
            bool ok = std::find(fail_pages.begin(), fail_pages.end(), p) == fail_pages.end();
            opts.on_page_progress(p, total_pages, ok);  // may throw CancellationException
        }
    });
    return stub;
}

class DocumentProcessorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(mgr_.Init(":memory:").ok());
        factory_ = std::make_unique<spc::DocumentParserFactory>(cfg_factory_);
        // The F06 factory stat()s the file in its pre-check (kFileNotFound
        // otherwise), so seed a real temp .pdf the injected stub will "parse".
        // Unique per test: parallel ctest processes must not share the stub
        // (a sibling's TearDown/remove yanks it mid-parse; F-1 race family).
        filepath_ = std::string(::testing::TempDir()) + "f42_proc_test_" +
                    cortrix::test::SanitizedTestName() +
                    ".pdf";
        std::ofstream(filepath_) << "%PDF-1.4 stub";
    }
    void TearDown() override { std::remove(filepath_.c_str()); }

    // Create a queued task, mark it processing (as the worker would), return it.
    TaskInfo SeedProcessingTask(const std::string& doc = "docA") {
        TaskInfo t;
        t.namespace_id = "ns1";
        t.filename = "big.pdf";
        t.filepath = filepath_;
        t.doc_id = doc;
        t.content_hash = "h1";
        auto c = mgr_.CreateTask(t);
        EXPECT_TRUE(c.ok());
        EXPECT_TRUE(mgr_.MarkProcessing(c.value().task_id, 1).ok());
        return mgr_.GetTask(c.value().task_id).value();
    }

    TaskManager mgr_;
    InMemoryGlobalConfig cfg_;
    spc::ParserFactoryConfig cfg_factory_;
    std::unique_ptr<spc::DocumentParserFactory> factory_;
    std::string filepath_;
};

TEST_F(DocumentProcessorTest, SuccessPersistsProgressAndCompletes) {
    factory_->SetPrimaryParser(MakeDrivingStub(3, MakeOnePageDoc("docling", 0.9f)));
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_);

    TaskInfo task = SeedProcessingTask("docA");
    Status s = proc.ProcessTask(task);
    EXPECT_TRUE(s.ok());

    auto got = mgr_.GetTask(task.task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, std::string(task_status::kCompleted));
    EXPECT_EQ(got.value().doc_id, "docA");
    EXPECT_FLOAT_EQ(got.value().progress_pct, 100.0f);  // MarkCompleted sets 100
    // last persisted progress reached page 3/3
    EXPECT_EQ(got.value().processed_pages, 3);
    EXPECT_EQ(got.value().total_pages, 3);
}

TEST_F(DocumentProcessorTest, FailedPagesRecordedDuringProgress) {
    factory_->SetPrimaryParser(MakeDrivingStub(4, MakeOnePageDoc("docling", 0.9f),
                                               /*fail_pages=*/{2}));
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_);
    TaskInfo task = SeedProcessingTask("docB");
    ASSERT_TRUE(proc.ProcessTask(task).ok());

    // failed_pages persisted mid-stream; final state still completed (per-page fault tolerance).
    auto got = mgr_.GetTask(task.task_id);
    EXPECT_EQ(got.value().processed_pages, 4);
    EXPECT_EQ(got.value().failed_pages, (std::vector<int>{2}));
}

TEST_F(DocumentProcessorTest, SetsParsingPhaseDuringProgress) {
    factory_->SetPrimaryParser(MakeDrivingStub(2, MakeOnePageDoc("docling", 0.9f)));
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_);
    TaskInfo task = SeedProcessingTask("docPhase");
    proc.ProcessTask(task);
    // current_phase is set to "parsing" (Issue 5) during progress persistence and
    // retained through MarkCompleted (the D3 wiring only runs the parse phase).
    auto got = mgr_.GetTask(task.task_id);
    EXPECT_EQ(got.value().current_phase, std::string(task_phase::kParsing));
}

TEST_F(DocumentProcessorTest, CancelAtCheckpointFinalizesCancelled) {
    // Drive 10 pages, but the cancel checker flips true after page 3 is observed.
    int pages_seen = 0;
    factory_->SetPrimaryParser(MakeDrivingStub(10, MakeOnePageDoc("docling", 0.9f)));
    // Model the real cancel: when the request lands it moves the row
    // processing -> cancelling (RequestCancel) and the checker observes it —
    // the guarded MarkCancelled only accepts queued|cancelling rows.
    DocumentProcessor::CancelChecker checker =
        [&pages_seen, this](const std::string& task_id) {
            // cancel becomes true once we've already processed 3 pages
            if (++pages_seen == 4) mgr_.RequestCancel(task_id, nullptr);
            return pages_seen > 3;
        };
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_, checker);

    TaskInfo task = SeedProcessingTask("docCancel");
    Status s = proc.ProcessTask(task);
    EXPECT_FALSE(s.ok());  // returns the CX_ERR_TASK_CANCELLING identity Status
    EXPECT_NE(s.message().find("CX_ERR_TASK_CANCELLING"), std::string::npos);

    auto got = mgr_.GetTask(task.task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCancelled));
}

TEST_F(DocumentProcessorTest, CancelAfterParseStillCancels) {
    // No cancel observed during pages, but the flag flips true by the time parsing
    // finishes (cancel arrived on the very last page). The post-parse re-check wins.
    bool cancel = false;
    TaskInfo task = SeedProcessingTask("docLate");
    auto stub = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
    stub->SetResult(MakeOnePageDoc("docling", 0.9f));
    stub->SetOnParse([&cancel, &task, this](const std::string&, const ParserOptions& opts) {
        if (opts.on_page_progress) {
            opts.on_page_progress(1, 2, true);
            opts.on_page_progress(2, 2, true);
        }
        // Cancel lands after the last page: processing -> cancelling in the DB,
        // observed only at the post-parse re-check.
        mgr_.RequestCancel(task.task_id, nullptr);
        cancel = true;
    });
    factory_->SetPrimaryParser(std::move(stub));
    DocumentProcessor::CancelChecker checker = [&cancel](const std::string&) { return cancel; };
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_, checker);
    Status s = proc.ProcessTask(task);
    EXPECT_FALSE(s.ok());
    auto got = mgr_.GetTask(task.task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kCancelled));
}

TEST_F(DocumentProcessorTest, ProgressPersistFailureIsToleratedDuringParse) {
    // A task row that vanished after dequeue (e.g. concurrent delete): the
    // per-page UpdateProgress returns NotFound; the processor logs a warning and
    // keeps parsing rather than aborting (the warn branch in on_page_progress).
    factory_->SetPrimaryParser(MakeDrivingStub(3, MakeOnePageDoc("docling", 0.9f)));
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_,
                           /*cancel_checker=*/[](const std::string&) { return false; });
    TaskInfo phantom;             // never inserted → UpdateProgress will NotFound
    phantom.task_id = "phantom-task";
    phantom.filepath = filepath_;
    phantom.doc_id = "docPhantom";
    // ProcessTask runs the parse + drives progress (each persist fails silently),
    // then MarkCompleted also NotFounds — the processor still returns Ok (parse OK)
    // because finalize errors are non-fatal to the parse outcome.
    Status s = proc.ProcessTask(phantom);
    EXPECT_TRUE(s.ok());  // parse succeeded; persistence failures were tolerated
}

TEST_F(DocumentProcessorTest, ParseErrorMapsToParseFailed) {
    // Stub returns an error ParsedDoc (no progress driven) → MarkFailed.
    ParsedDoc err = spc::MakeErrorDoc(ParserErrorCode::kSubprocessFailed, "boom",
                                      nlohmann::json::object(), "docling");
    auto stub = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
    stub->SetResult(err);
    factory_->SetPrimaryParser(std::move(stub));
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_);

    TaskInfo task = SeedProcessingTask("docErr");
    Status s = proc.ProcessTask(task);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PARSE_FAILED"), std::string::npos);

    auto got = mgr_.GetTask(task.task_id);
    EXPECT_EQ(got.value().status, std::string(task_status::kFailed));
    EXPECT_EQ(got.value().error_code, "CX_ERR_PARSE_FAILED");
    EXPECT_NE(got.value().structured_data.find("parser_error"), std::string::npos);
}

TEST_F(DocumentProcessorTest, UsesEntMaxPagesFromConfig) {
    int seen_max = -1;
    auto stub = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
    stub->SetResult(MakeOnePageDoc("docling", 0.9f));
    stub->SetOnParse([&seen_max](const std::string&, const ParserOptions& opts) {
        seen_max = opts.max_pages;
        if (opts.on_page_progress) opts.on_page_progress(1, 1, true);
    });
    factory_->SetPrimaryParser(std::move(stub));
    cfg_.Set("f42.async_max_pages", "1234");
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_);
    proc.ProcessTask(SeedProcessingTask("docMax"));
    EXPECT_EQ(seen_max, 1234);  // Issue 1.3 — async page cap from config
}

TEST_F(DocumentProcessorTest, DefaultEntMaxPagesWhenConfigAbsent) {
    int seen_max = -1;
    auto stub = std::make_unique<StubParser>("docling", std::vector<std::string>{"pdf"});
    stub->SetResult(MakeOnePageDoc("docling", 0.9f));
    stub->SetOnParse([&seen_max](const std::string&, const ParserOptions& opts) {
        seen_max = opts.max_pages;
        if (opts.on_page_progress) opts.on_page_progress(1, 1, true);
    });
    factory_->SetPrimaryParser(std::move(stub));
    DocumentProcessor proc(&mgr_, factory_.get(), nullptr);  // no config
    proc.ProcessTask(SeedProcessingTask("docDef"));
    EXPECT_EQ(seen_max, DocumentProcessor::kDefaultAsyncMaxPages);  // 2000
}

// [Plan B · F42 §4.1.2] Wired to SPC: a successful parse hands the ParsedDoc to
// SPCManager::ProcessParsedDoc, and rc=0 finalizes the task completed.
TEST_F(DocumentProcessorTest, WiredToSpcHandsParsedDocToProcessParsedDoc) {
    factory_->SetPrimaryParser(MakeDrivingStub(3, MakeOnePageDoc("docling", 0.9f)));
    ::testing::NiceMock<cortrix::testing::MockSPCManager> spc;
    EXPECT_CALL(spc, ProcessParsedDoc(::testing::_, ::testing::_))
        .WillOnce(::testing::Return(0));
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_, nullptr, &spc);

    TaskInfo task = SeedProcessingTask("docSpc");
    Status s = proc.ProcessTask(task);
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(mgr_.GetTask(task.task_id).value().status,
              std::string(task_status::kCompleted));
}

// SPC post-parse failure (rc=-1) finalizes the task failed with CX_ERR_SPC_PROCESS_FAILED.
TEST_F(DocumentProcessorTest, SpcProcessFailureFinalizesFailed) {
    factory_->SetPrimaryParser(MakeDrivingStub(3, MakeOnePageDoc("docling", 0.9f)));
    ::testing::NiceMock<cortrix::testing::MockSPCManager> spc;
    EXPECT_CALL(spc, ProcessParsedDoc(::testing::_, ::testing::_))
        .WillOnce([](ParsedDoc&, SPCTask& t) { t.error_message = "chunk failed"; return -1; });
    DocumentProcessor proc(&mgr_, factory_.get(), &cfg_, nullptr, &spc);

    TaskInfo task = SeedProcessingTask("docSpcFail");
    Status s = proc.ProcessTask(task);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_SPC_PROCESS_FAILED"), std::string::npos);
    EXPECT_EQ(mgr_.GetTask(task.task_id).value().status,
              std::string(task_status::kFailed));
}

}  // namespace
}  // namespace cortrix::async
