// Unit tests for MemoryExtractionService (memory_extraction_service.cpp).
//
// Coverage target: the 5 ExtractOne() branches (lines 52-119) that are at 0%
// in the core-17 line-coverage report, pulling mem-extraction-service from 0%
// toward the D4 >= 90% line-coverage gate.
//
// Branch map (see memory_extraction_service.cpp):
//   Branch 1 (line 55-58):  !enabled() — llm=nullptr => benign success, no extraction
//   Branch 2 (line 66-69):  facade.Acquire() fails (un-admitted namespace) => skip, ok
//   Branch 3 (line 91-95):  interaction.remember==false => opt-out + skip, ok
//   Branch 4 (line 99-103): session already opted-out => skip, ok
//   Branch 5 (line 109-117): real extraction path:
//                              5a: PushOk  => result.ok() == true
//                              5b: PushError => result.ok() == false (covers lines 114-117)
//
// Also tested: enabled() true/false; Enqueue disabled=>false / enabled=>true;
// Start/Stop idempotency; queue() accessor.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <queue>
#include <string>

#include "cortrix/memory/interaction_log.h"
#include "cortrix/memory/memory_extraction_service.h"
#include "cortrix/memory/memory_extractor.h"
#include "cortrix/memory/memory_block_adapter.h"   // seed/read real NS memory blocks
#include "cortrix/memory/memory_queue.h"
#include "cortrix/memory/memory_session.h"
#include "cortrix/llm/i_llm_client.h"
#include "cortrix/common/status.h"
#include "cortrix/observability/operation_logger.h"  // CountingOpLogger (revoke audit)
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/onnx_embedder.h"
#include "ns_pool_test_helper.h"

namespace cortrix::memory {
namespace {

// ---------------------------------------------------------------------------
// ScriptedLlmClient — local copy of the scriptable test double from
// test_memory_extractor.cpp (which lives in an anonymous namespace and cannot
// be #included). API is intentionally identical so future refactors align.
// ---------------------------------------------------------------------------
class ScriptedLlmClient : public llm::ILlmClient {
public:
    void PushOk(const std::string& content, int prompt_tokens = 100,
                int completion_tokens = 40,
                const std::string& model = "gpt-4o-mini") {
        llm::ChatCompletionResponse r;
        r.status  = Status::Ok();
        r.content = content;
        r.model   = model;
        r.finish_reason   = "stop";
        r.prompt_tokens   = prompt_tokens;
        r.completion_tokens = completion_tokens;
        responses_.push(r);
    }

    void PushError(StatusCode code, const std::string& msg) {
        llm::ChatCompletionResponse r;
        r.status = Status(code, msg);
        responses_.push(r);
    }

    llm::ChatCompletionResponse Chat(const std::string& prompt,
                                     const llm::LlmCallConfig& config) override {
        (void)prompt;
        (void)config;
        ++call_count;
        if (responses_.empty()) {
            llm::ChatCompletionResponse r;
            r.status = Status::Internal("ScriptedLlmClient: no scripted response");
            return r;
        }
        auto r = responses_.front();
        responses_.pop();
        return r;
    }

    int call_count = 0;

private:
    std::queue<llm::ChatCompletionResponse> responses_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a minimal InteractionLog suitable for extraction tests.
// By default remember=true so it passes the MEM04 double-check.
InteractionLog MakeInteraction(const std::string& ns,
                               const std::string& session_id,
                               const std::string& id = "ilog-001",
                               bool remember = true) {
    InteractionLog log;
    log.id             = id;
    log.session_id     = session_id;
    log.namespace_name = ns;
    log.user_id        = "user-test";
    log.role           = "user";
    log.content        = "I am a software engineer based in Singapore.";
    log.query_type     = "chat";
    log.status         = "success";
    log.remember       = remember;
    return log;
}

// Valid single-memory JSON the MemoryExtractor accepts.
constexpr const char* kValidExtractionJson =
    R"([{"type":"fact","content":"user is a software engineer","confidence":0.9}])";

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class MemoryExtractionServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique temp directory per test process (avoids inter-test collisions).
        tmp_dir_ = std::string("/tmp/cortrix_mes_test_") + std::to_string(getpid());
        std::filesystem::create_directories(tmp_dir_);

        // Stand up a real DefaultNamespacePool in the temp dir.
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
            tmp_dir_ + "/pool");

        // Admit "default" — used by all tests that need a live namespace.
        ASSERT_TRUE(harness_->Admit("default").ok());

        // OnnxEmbedder in stub mode (matches test_memory_routes.cpp pattern).
        embedder_ = std::make_unique<OnnxEmbedder>("stub_model.onnx", 128);
        embedder_->Init();
    }

    void TearDown() override {
        harness_.reset();  // destroys pool + closes DBs before rm
        std::filesystem::remove_all(tmp_dir_);
    }

    // Build a service with the supplied LLM (nullptr = disabled mode).
    std::unique_ptr<MemoryExtractionService> MakeService(
            std::shared_ptr<llm::ILlmClient> llm) {
        return std::make_unique<MemoryExtractionService>(
            harness_->ipool(),
            std::move(llm),
            *embedder_,
            /*op_logger=*/nullptr,   // skip_oplog guard: if (!op_logger_) return;
            MemoryExtractorConfig{},
            MemoryQueue::Config{});
    }

    std::string tmp_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::unique_ptr<OnnxEmbedder> embedder_;
};

// ---------------------------------------------------------------------------
// enabled() — basic predicate
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, EnabledFalseWhenLlmNull) {
    auto svc = MakeService(nullptr);
    EXPECT_FALSE(svc->enabled());
}

TEST_F(MemoryExtractionServiceTest, EnabledTrueWhenLlmProvided) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    auto svc = MakeService(llm);
    EXPECT_TRUE(svc->enabled());
}

// ---------------------------------------------------------------------------
// Enqueue — disabled path and enabled path
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, EnqueueReturnsFalseWhenDisabled) {
    auto svc = MakeService(nullptr);
    auto log = MakeInteraction("default", "session-abc");
    EXPECT_FALSE(svc->Enqueue(log));
}

TEST_F(MemoryExtractionServiceTest, EnqueueReturnsTrueWhenEnabled) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    auto svc = MakeService(llm);
    // Start workers so the queue is live (Enqueue -> queue_.Push).
    svc->Start();
    auto log = MakeInteraction("default", "session-abc");
    EXPECT_TRUE(svc->Enqueue(log));
    svc->Stop();
}

// ---------------------------------------------------------------------------
// queue() accessor
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, QueueAccessorIsAccessible) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    auto svc = MakeService(llm);
    // Just confirm it compiles and returns a MemoryQueue reference.
    MemoryQueue& q = svc->queue();
    EXPECT_EQ(q.Depth(), 0u);
}

// ---------------------------------------------------------------------------
// Start / Stop idempotency
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, StartStopIdempotentNocrash) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    auto svc = MakeService(llm);
    // Double-Start should be a no-op (started_ guard).
    svc->Start();
    svc->Start();
    // Double-Stop should also be a no-op (started_ guard in Stop).
    svc->Stop();
    svc->Stop();
    // No assertions needed — just verifying no crash / deadlock.
}

TEST_F(MemoryExtractionServiceTest, StartNoopWhenDisabled) {
    // When llm=nullptr, Start() returns early (line 27: if (started_ || !enabled())).
    auto svc = MakeService(nullptr);
    svc->Start();
    svc->Stop();  // also a no-op (started_==false)
}

// ---------------------------------------------------------------------------
// Branch 1: ExtractOne — disabled (llm=nullptr) => benign success
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, ExtractOne_Branch1_DisabledReturnsBenignSuccess) {
    auto svc = MakeService(nullptr);
    ASSERT_FALSE(svc->enabled());

    auto log = MakeInteraction("default", "session-b1");
    MemoryExtractionResult result = svc->ExtractOne(log);

    // Disabled path: a default-constructed result (no error) => ok()==true.
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.extracted_memories.empty());
}

// ---------------------------------------------------------------------------
// Branch 2: ExtractOne — namespace not admitted => facade.Acquire() fails => skip ok
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, ExtractOne_Branch2_UnadmittedNsSkipsOk) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    auto svc = MakeService(llm);
    ASSERT_TRUE(svc->enabled());

    // Use a namespace name that was never Admit()ted to the pool.
    auto log = MakeInteraction("ns-does-not-exist", "session-b2");
    MemoryExtractionResult result = svc->ExtractOne(log);

    // Unavailable namespace is not retryable => skip with ok()==true.
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.extracted_memories.empty());
    // The scripted LLM should not have been called.
    EXPECT_EQ(llm->call_count, 0);
}

// ---------------------------------------------------------------------------
// Branch 3: ExtractOne — interaction.remember==false => opt-out + skip ok
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, ExtractOne_Branch3_RememberFalseSkipsOk) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    auto svc = MakeService(llm);
    ASSERT_TRUE(svc->enabled());

    // remember=false triggers MEM04 D2 opt-out and immediate skip.
    auto log = MakeInteraction("default", "session-b3",
                               /*id=*/"ilog-b3", /*remember=*/false);
    MemoryExtractionResult result = svc->ExtractOne(log);

    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.extracted_memories.empty());
    EXPECT_EQ(llm->call_count, 0);
}

// ---------------------------------------------------------------------------
// Branch 4: ExtractOne — session already opted-out => skip ok
//
// Strategy:
//   1. Open a NamespaceFacade directly to obtain the same per-NS MemoryStore
//      that ExtractOne will later use for its opt-out check.
//   2. Call SessionCreate() so the session row exists in memory.db.
//      SessionCreate auto-generates a UUID session_id, which we capture.
//   3. Call ExtractOne with remember=false using that session_id (Branch 3):
//      OptOutManager::OptOut() can now find the row and stamp opted-out.
//   4. Call ExtractOne again with remember=true and the same session_id:
//      OptOutManager::is_session_opted_out() returns true => Branch 4 skips.
//
// Note: NamespaceFacade owns its own memory.db connection (D-I4 light resource),
// but it is rooted at the same on-disk <unit_dir>/memory.db that ExtractOne's
// internal facade will also open.  Both facades therefore see the same persisted
// opt-out stamp through SQLite WAL.
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, ExtractOne_Branch4_AlreadyOptedOutSkipsOk) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    auto svc = MakeService(llm);
    ASSERT_TRUE(svc->enabled());

    const std::string kNs = "default";

    // Step 1+2: open a facade to the NS and create the session row so that
    // OptOutManager::OptOut() can locate and stamp it.
    std::string session_id;
    {
        resource::NamespaceFacade setup_facade(harness_->ipool(), kNs);
        ASSERT_TRUE(setup_facade.Acquire().ok());

        MemorySession session;
        session.namespace_name = kNs;
        ASSERT_TRUE(setup_facade.memory().SessionCreate(session).ok());
        // Capture the auto-generated UUID session_id.
        session_id = session.session_id;
        ASSERT_FALSE(session_id.empty());
        // facade destructor releases the bundle back to the pool here.
    }

    // Step 3: remember=false => Branch 3 => OptOutManager stamps the session
    // opted-out in the on-disk memory.db.
    {
        auto log = MakeInteraction(kNs, session_id, "ilog-b4-a", /*remember=*/false);
        MemoryExtractionResult r = svc->ExtractOne(log);
        ASSERT_TRUE(r.ok()) << "Branch 3 (remember=false) must succeed as precondition";
        EXPECT_EQ(llm->call_count, 0);
    }

    // Step 4: remember=true, same session => Branch 4 detects existing opt-out,
    // skips extraction, returns benign ok().
    {
        auto log = MakeInteraction(kNs, session_id, "ilog-b4-b", /*remember=*/true);
        MemoryExtractionResult r = svc->ExtractOne(log);
        EXPECT_TRUE(r.ok());
        EXPECT_TRUE(r.extracted_memories.empty());
        EXPECT_EQ(llm->call_count, 0);  // no LLM call in Branch 3 or Branch 4
    }
}

// ---------------------------------------------------------------------------
// Branch 5a: ExtractOne — real extraction, LLM succeeds => result.ok() true
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, ExtractOne_Branch5a_SuccessfulExtraction) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    // Queue a valid extraction response.
    llm->PushOk(kValidExtractionJson);

    auto svc = MakeService(llm);
    ASSERT_TRUE(svc->enabled());

    auto log = MakeInteraction("default", "session-b5a");
    MemoryExtractionResult result = svc->ExtractOne(log);

    EXPECT_TRUE(result.ok());
    // At least one memory extracted.
    EXPECT_FALSE(result.extracted_memories.empty());
    EXPECT_EQ(result.extracted_memories[0].type, MemoryType::kFact);
    EXPECT_GE(llm->call_count, 1);
}

// ---------------------------------------------------------------------------
// Branch 5b: ExtractOne — real extraction, LLM returns error => result non-ok
//           Covers the CORTRIX_LOG_WARN path at lines 114-117.
// ---------------------------------------------------------------------------

TEST_F(MemoryExtractionServiceTest, ExtractOne_Branch5b_LlmErrorResultNotOk) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    // Push a timeout-style error; MemoryExtractor maps this to
    // CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT with retryable=true.
    llm->PushError(StatusCode::kUnavailable, "circuit open / timeout");

    auto svc = MakeService(llm);
    ASSERT_TRUE(svc->enabled());

    auto log = MakeInteraction("default", "session-b5b");
    MemoryExtractionResult result = svc->ExtractOne(log);

    // Extraction failed => the warn-log branch (lines 114-117) executed.
    EXPECT_FALSE(result.ok());
    ASSERT_TRUE(result.error.has_value());
    // MemoryExtractor maps kUnavailable to the LLM_TIMEOUT error code.
    EXPECT_EQ(result.error->code, "CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT");
    EXPECT_GE(llm->call_count, 1);
}

// ---------------------------------------------------------------------------
// RevokeInvalidation (#22 D9 admin revoke) — real NS dependency stack
// ---------------------------------------------------------------------------

// Minimal IOperationLogger recorder (local; the test_memory_extractor.cpp copy is in
// an anonymous namespace and cannot be #included).
class CountingOpLogger : public observability::IOperationLogger {
public:
    void Log(const observability::OperationLogEntry& e,
             const observability::TraceContext* = nullptr) override { logged.push_back(e); }
    void BatchLog(const std::vector<observability::OperationLogEntry>& es,
                  const observability::TraceContext* = nullptr) override {
        for (const auto& e : es) logged.push_back(e);
    }
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext* = nullptr) override {
        return observability::OperationLogQueryResult{};
    }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }
    int CountAction(const std::string& a) const {
        int n = 0; for (const auto& e : logged) if (e.action == a) ++n; return n;
    }
    std::vector<observability::OperationLogEntry> logged;
};

// Real-dependency-stack revoke: seed an invalidated block into the live "default" NS
// store via the same MemoryBlockAdapter the service uses, revoke through the service,
// then read the block back from the store and assert it is active again + the audit
// entry was written. (A D3.5-mounted wiring is only proven by the real dependency
// stack, not a mocked store — single-unit tests can pass while live assembly is broken.)
TEST_F(MemoryExtractionServiceTest, RevokeInvalidationRestoresActiveOnRealStore) {
    auto oplog = std::make_shared<CountingOpLogger>();
    // A service with a real op_logger so the memory_revoke audit entry is observable.
    auto svc = std::make_unique<MemoryExtractionService>(
        harness_->ipool(), /*llm=*/nullptr, *embedder_, oplog,
        MemoryExtractorConfig{}, MemoryQueue::Config{});

    // Seed an invalidated memory block into the live NS store.
    resource::NamespaceFacade facade(harness_->ipool(), "default");
    ASSERT_TRUE(facade.Acquire().ok());
    MemoryBlockAdapter seed_store(facade.store(), embedder_.get(), &facade.vec_index());
    MemoryBlockRecord b;
    b.block_id = "blk_revoke_svc";  // explicit id (InsertMemoryBlock echoes it back)
    b.ns_id = "default";
    b.user_id = "user_123";
    b.content = "user is in Shanghai";
    b.metadata_json = {{"memory_type", "fact"}, {"status", "invalidated"},
                       {"user_id", "user_123"}};
    auto ins = seed_store.InsertMemoryBlock(b);
    ASSERT_TRUE(ins.ok());
    const std::string block_id = ins.value();
    ASSERT_FALSE(block_id.empty());

    // Revoke through the service (NS-scoped extractor built internally).
    auto r = svc->RevokeInvalidation("default", block_id, "business trip not a move",
                                     /*revoked_by=*/"admin_op");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().metadata_json.value("status", ""), "active");
    EXPECT_EQ(r.value().metadata_json.value("revoked_by", ""), "admin_op");

    // Read back from the live store: the persisted block must be active now.
    auto got = seed_store.GetMemoryBlock(block_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().metadata_json.value("status", ""), "active");

    // The D9 revoke wrote exactly one memory_revoke audit entry.
    EXPECT_EQ(oplog->CountAction("memory_revoke"), 1);
}

// A revoke against a non-existent namespace is a NotFound (CX_ERR_NS_NOT_FOUND),
// not a crash — the façade acquire fails before any store work.
TEST_F(MemoryExtractionServiceTest, RevokeUnknownNamespaceIsNotFound) {
    auto svc = MakeService(nullptr);
    auto r = svc->RevokeInvalidation("no_such_ns", "blk", "x", "admin_op");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
}

// Revoke works even when the service has no LLM (it is a pure store transition,
// unlike ExtractOne which short-circuits when disabled).
TEST_F(MemoryExtractionServiceTest, RevokeMissingBlockReturnsNotFoundWithoutLlm) {
    auto svc = MakeService(nullptr);  // disabled (no LLM)
    auto r = svc->RevokeInvalidation("default", "no_such_block", "x", "admin_op");
    ASSERT_FALSE(r.ok());  // block not found, but it DID reach the store (not a disabled-skip)
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace cortrix::memory
