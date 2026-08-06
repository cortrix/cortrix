#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/memory/mem04_error.h"
#include "cortrix/memory/mem04_metrics.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/opt_out_manager.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/store/cortrix_store.h"

// S2-S6 coverage: OptOutManager (the memory opt-out main service class) over a mock
// ISessionOptOutStore + a capturing IOperationLogger, plus an end-to-end pass through
// the real MemoryStoreOptOutAdapter (proves the §4.1/§4.2 migration + the three store
// ops + the adapter all work against real SQLite). Standalone-D3: no cross-process
// Memory extraction wiring (that is integration).
namespace cortrix::memory::immunity {
namespace {

using observability::HealthStatus;
using observability::IOperationLogger;
using observability::OperationLogEntry;
using observability::OperationLogFilter;
using observability::OperationLogQueryResult;
using observability::OperationLogStats;
using observability::TraceContext;

constexpr char kValidSession[] = "11111111-2222-4333-8444-555566667777";  // UUID v4-ish

// ---- Mock opt-out store (in-memory, deterministic) ----
class FakeOptOutStore : public ISessionOptOutStore {
public:
    // Pre-register a session row (active). Absent ids report exists=false.
    void AddSession(const std::string& id) { sessions_[id] = State{}; }

    void ForceStoreError(bool on) { fail_ = on; }
    // Fail only the mutating ops (SetOptOut / ClearOptOut), so GetOptOutState still
    // succeeds and the manager reaches the write — exercising the write-failure return.
    void ForceWriteError(bool on) { write_fail_ = on; }

    Result<SessionOptOutState> GetOptOutState(const std::string& id) override {
        if (fail_) return Status::Internal("forced store error");
        SessionOptOutState st;
        auto it = sessions_.find(id);
        if (it == sessions_.end()) { st.exists = false; return st; }
        st.exists = true;
        st.opted_out = it->second.opted_out;
        st.opt_out_at = it->second.opt_out_at;
        st.opted_out_by = it->second.opted_out_by;
        return st;
    }

    Status SetOptOut(const std::string& id, const std::string& at,
                     const std::string& by) override {
        if (fail_ || write_fail_) return Status::Internal("forced store error");
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return Status::NotFound("no session");
        it->second.opted_out = true;
        it->second.opt_out_at = at;
        it->second.opted_out_by = by;
        return Status::Ok();
    }

    Status ClearOptOut(const std::string& id) override {
        if (fail_ || write_fail_) return Status::Internal("forced store error");
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return Status::NotFound("no session");
        it->second.opted_out = false;
        it->second.opt_out_at.clear();
        it->second.opted_out_by.clear();
        return Status::Ok();
    }

    bool IsOptedOut(const std::string& id) const {
        auto it = sessions_.find(id);
        return it != sessions_.end() && it->second.opted_out;
    }

private:
    struct State {
        bool opted_out = false;
        std::string opt_out_at;
        std::string opted_out_by;
    };
    std::map<std::string, State> sessions_;
    bool fail_ = false;
    bool write_fail_ = false;
};

// ---- Capturing operation logger (records what was logged) ----
class CapturingOpLogger : public IOperationLogger {
public:
    void Log(const OperationLogEntry& entry, const TraceContext* = nullptr) override {
        entries.push_back(entry);
    }
    void BatchLog(const std::vector<OperationLogEntry>& es, const TraceContext* = nullptr) override {
        for (const auto& e : es) entries.push_back(e);
    }
    Result<OperationLogQueryResult> Query(const OperationLogFilter&,
                                          const TraceContext* = nullptr) override {
        return OperationLogQueryResult{};
    }
    void Cleanup() override {}
    OperationLogStats GetStats() override { return {}; }
    HealthStatus Health() override { return {}; }

    std::vector<OperationLogEntry> entries;
};

// ---- Minimal CortrixStore stub (MemoryStore only needs the reference) ----
class NullCortrixStore : public CortrixStore {
public:
    int Open() override { return 0; }
    int Close() override { return 0; }
    int doc_create(CortrixDoc&) override { return 0; }
    int doc_get(const std::string&, CortrixDoc&) override { return -2; }
    int doc_update_status(const std::string&, DocStatus, const std::string&) override { return 0; }
    int doc_delete(const std::string&) override { return 0; }
    int doc_list_by_status(DocStatus, std::vector<CortrixDoc>&) override { return 0; }
    int doc_find_by_source(const std::string&, const std::string&, CortrixDoc&) override { return -2; }
    int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }
    int doc_count(int64_t* c) override { *c = 0; return 0; }
    int block_insert(CortrixBlock&) override { return 0; }
    int block_get(uint64_t, CortrixBlock&) override { return -2; }
    int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t* c) override { *c = 0; return 0; }
    int search_fulltext(const std::string&, int, std::vector<SearchResult>&) override { return 0; }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

OptOutManager MakeManager(std::shared_ptr<FakeOptOutStore> store,
                          std::shared_ptr<IOperationLogger> logger = nullptr,
                          bool enabled = true) {
    return OptOutManager(std::move(store), std::move(logger), enabled);
}

// ============================ IsValidSessionId =============================

TEST(OptOutManagerValidateTest, AcceptsUuidAndUlid) {
    EXPECT_TRUE(OptOutManager::IsValidSessionId(kValidSession));
    EXPECT_TRUE(OptOutManager::IsValidSessionId("01ARZ3NDEKTSV4RRFFQ69G5FAV"));  // ULID
    EXPECT_TRUE(OptOutManager::IsValidSessionId("s_1234567890"));
}

TEST(OptOutManagerValidateTest, RejectsEmptyShortAndJunk) {
    EXPECT_FALSE(OptOutManager::IsValidSessionId(""));
    EXPECT_FALSE(OptOutManager::IsValidSessionId("short"));        // < 8
    EXPECT_FALSE(OptOutManager::IsValidSessionId("has spaces!!"));  // bad charset
    EXPECT_FALSE(OptOutManager::IsValidSessionId(std::string(65, 'a')));  // > 64
}

TEST(OptOutManagerValidateTest, ActorStrings) {
    EXPECT_STREQ(OptOutManager::ActorString(OptOutActor::kUserManual), "user_manual");
    EXPECT_STREQ(OptOutManager::ActorString(OptOutActor::kAgentAuto), "agent_auto");
    EXPECT_STREQ(OptOutManager::ActorString(OptOutActor::kSystemAuto), "system_auto");
    EXPECT_STREQ(OptOutManager::ActorString(OptOutActor::kTest), "test");
    EXPECT_STREQ(ToString(OptOutActor::kAgentAuto), "agent_auto");  // free fn delegates
}

// ================================ OptOut ==================================

class OptOutManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_shared<FakeOptOutStore>();
        logger_ = std::make_shared<CapturingOpLogger>();
        Mem04Metrics::Instance().ResetForTest();
    }
    void TearDown() override { Mem04Metrics::Instance().ResetForTest(); }

    std::shared_ptr<FakeOptOutStore> store_;
    std::shared_ptr<CapturingOpLogger> logger_;
};

TEST_F(OptOutManagerTest, OptOutSuccessStampsAndLogsAndMeters) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_);

    auto r = mgr.OptOut(kValidSession, OptOutActor::kUserManual, "privacy chat");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r->session_id, kValidSession);
    EXPECT_FALSE(r->opt_out_at.empty());
    EXPECT_EQ(r->opted_out_by, "user_manual");
    EXPECT_TRUE(store_->IsOptedOut(kValidSession));

    // metric
    EXPECT_EQ(Mem04Metrics::Instance().OptOutCount(Mem04Metrics::TriggeredBy::kUser), 1u);

    // operation_log: action memory_opt_out, resource memory/{session}, summary w/ reason
    ASSERT_EQ(logger_->entries.size(), 1u);
    EXPECT_EQ(logger_->entries[0].action, "memory_opt_out");
    EXPECT_EQ(logger_->entries[0].resource_type, "memory");
    ASSERT_TRUE(logger_->entries[0].resource_id.has_value());
    EXPECT_EQ(*logger_->entries[0].resource_id, kValidSession);
    ASSERT_TRUE(logger_->entries[0].summary.has_value());
    EXPECT_NE(logger_->entries[0].summary->find("privacy chat"), std::string::npos);
}

TEST_F(OptOutManagerTest, OptOutAgentActorMapsToAgentMetric) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_);
    ASSERT_TRUE(mgr.OptOut(kValidSession, OptOutActor::kAgentAuto).ok());
    EXPECT_EQ(Mem04Metrics::Instance().OptOutCount(Mem04Metrics::TriggeredBy::kAgent), 1u);
}

TEST_F(OptOutManagerTest, OptOutSystemActorMapsToSystemMetric) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_);
    ASSERT_TRUE(mgr.OptOut(kValidSession, OptOutActor::kSystemAuto).ok());
    EXPECT_EQ(Mem04Metrics::Instance().OptOutCount(Mem04Metrics::TriggeredBy::kSystem), 1u);
}

TEST_F(OptOutManagerTest, OptOutTestActorMapsToSystemMetricAndStamps) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.OptOut(kValidSession, OptOutActor::kTest);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r->opted_out_by, "test");
    // MetricTrigger(kTest) collapses onto the system label (line 41).
    EXPECT_EQ(Mem04Metrics::Instance().OptOutCount(Mem04Metrics::TriggeredBy::kSystem), 1u);
}

TEST_F(OptOutManagerTest, OptOutInvalidSessionId) {
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.OptOut("bad id");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM04_INVALID_SESSION_ID"), std::string::npos);
    EXPECT_TRUE(logger_->entries.empty());  // rejected before any side effect
}

TEST_F(OptOutManagerTest, OptOutSessionNotFound) {
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.OptOut(kValidSession);  // not registered
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
    EXPECT_NE(r.status().message().find("CX_ERR_MEM04_SESSION_NOT_FOUND"), std::string::npos);
}

TEST_F(OptOutManagerTest, OptOutAlreadyOptedOut) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_);
    ASSERT_TRUE(mgr.OptOut(kValidSession).ok());
    auto r2 = mgr.OptOut(kValidSession);
    ASSERT_FALSE(r2.ok());
    EXPECT_NE(r2.status().message().find("CX_ERR_MEM04_ALREADY_OPTED_OUT"), std::string::npos);
    // only the first opt-out logged + metered
    EXPECT_EQ(logger_->entries.size(), 1u);
    EXPECT_EQ(Mem04Metrics::Instance().OptOutCount(Mem04Metrics::TriggeredBy::kUser), 1u);
}

TEST_F(OptOutManagerTest, OptOutDisabledReturns503Semantics) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_, /*enabled=*/false);
    auto r = mgr.OptOut(kValidSession);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kUnavailable);
    EXPECT_NE(r.status().message().find("CX_ERR_MEM04_OPT_OUT_DISABLED"), std::string::npos);
    EXPECT_FALSE(store_->IsOptedOut(kValidSession));
}

TEST_F(OptOutManagerTest, OptOutPropagatesStoreError) {
    store_->AddSession(kValidSession);
    store_->ForceStoreError(true);
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.OptOut(kValidSession);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
}

// OptOut reaches SetOptOut (state exists + active) but the write fails → the
// store error is returned (lines 128-130), with no metric/log side effects.
TEST_F(OptOutManagerTest, OptOutSetOptOutWriteFailureReturnsError) {
    store_->AddSession(kValidSession);
    store_->ForceWriteError(true);
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.OptOut(kValidSession);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
    EXPECT_TRUE(logger_->entries.empty());
    EXPECT_EQ(Mem04Metrics::Instance().OptOutCount(Mem04Metrics::TriggeredBy::kUser), 0u);
}

TEST_F(OptOutManagerTest, OptOutWorksWithoutLogger) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, /*logger=*/nullptr);  // standalone: no oplog wired
    auto r = mgr.OptOut(kValidSession);
    EXPECT_TRUE(r.ok()) << r.status().message();
    EXPECT_TRUE(store_->IsOptedOut(kValidSession));
}

// ============================== OptOutRevoke ==============================

TEST_F(OptOutManagerTest, RevokeSuccessClearsAndLogsAndMeters) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_);
    ASSERT_TRUE(mgr.OptOut(kValidSession).ok());
    logger_->entries.clear();

    auto r = mgr.OptOutRevoke(kValidSession, "user wants memories back");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r->session_id, kValidSession);
    EXPECT_FALSE(r->revoked_at.empty());
    EXPECT_FALSE(store_->IsOptedOut(kValidSession));

    EXPECT_EQ(Mem04Metrics::Instance().OptOutRevokeCount(), 1u);
    ASSERT_EQ(logger_->entries.size(), 1u);
    EXPECT_EQ(logger_->entries[0].action, "memory_opt_out_revoke");
    ASSERT_TRUE(logger_->entries[0].summary.has_value());
    EXPECT_NE(logger_->entries[0].summary->find("user wants memories back"), std::string::npos);
}

TEST_F(OptOutManagerTest, RevokeRequiresReason) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_);
    ASSERT_TRUE(mgr.OptOut(kValidSession).ok());
    auto r = mgr.OptOutRevoke(kValidSession, "");  // missing reason
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM04_INVALID_SESSION_ID"), std::string::npos);
}

TEST_F(OptOutManagerTest, RevokeInvalidSessionId) {
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.OptOutRevoke("bad id", "reason");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM04_INVALID_SESSION_ID"), std::string::npos);
}

TEST_F(OptOutManagerTest, RevokeSessionNotFound) {
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.OptOutRevoke(kValidSession, "reason");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM04_SESSION_NOT_FOUND"), std::string::npos);
}

TEST_F(OptOutManagerTest, RevokeNotOptedOut) {
    store_->AddSession(kValidSession);  // active, never opted out
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.OptOutRevoke(kValidSession, "reason");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM04_NOT_OPTED_OUT"), std::string::npos);
}

TEST_F(OptOutManagerTest, RevokeDisabledReturns503Semantics) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_, /*enabled=*/false);
    auto r = mgr.OptOutRevoke(kValidSession, "reason");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM04_OPT_OUT_DISABLED"), std::string::npos);
}

// Revoke passes validation + state checks (exists + opted_out) but ClearOptOut
// fails → the store error is returned (lines 171-173).
TEST_F(OptOutManagerTest, RevokeClearOptOutWriteFailureReturnsError) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_);
    ASSERT_TRUE(mgr.OptOut(kValidSession).ok());  // now opted out
    logger_->entries.clear();
    store_->ForceWriteError(true);  // ClearOptOut will now fail

    auto r = mgr.OptOutRevoke(kValidSession, "give it back");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
    EXPECT_TRUE(logger_->entries.empty());  // no revoke audit on failure
    EXPECT_EQ(Mem04Metrics::Instance().OptOutRevokeCount(), 0u);
}

// Revoke propagates a GetOptOutState store failure (line 162) before any write.
TEST_F(OptOutManagerTest, RevokePropagatesGetStateError) {
    store_->AddSession(kValidSession);
    store_->ForceStoreError(true);
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.OptOutRevoke(kValidSession, "reason");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
}

// =========================== is_session_opted_out =========================

TEST_F(OptOutManagerTest, IsOptedOutTrueAfterOptOut) {
    store_->AddSession(kValidSession);
    auto mgr = MakeManager(store_, logger_);
    auto before = mgr.is_session_opted_out(kValidSession);
    ASSERT_TRUE(before.ok());
    EXPECT_FALSE(*before);

    ASSERT_TRUE(mgr.OptOut(kValidSession).ok());
    auto after = mgr.is_session_opted_out(kValidSession);
    ASSERT_TRUE(after.ok());
    EXPECT_TRUE(*after);
}

TEST_F(OptOutManagerTest, IsOptedOutAbsentSessionIsFalseNotError) {
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.is_session_opted_out(kValidSession);  // no such session
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(*r);
}

TEST_F(OptOutManagerTest, IsOptedOutNotGatedByDisabled) {
    store_->AddSession(kValidSession);
    // opt out while enabled, then construct a disabled manager over the same store
    MakeManager(store_, logger_).OptOut(kValidSession);
    auto disabled = MakeManager(store_, logger_, /*enabled=*/false);
    auto r = disabled.is_session_opted_out(kValidSession);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(*r) << "existing opt-out must be honored even when feature disabled";
}

TEST_F(OptOutManagerTest, IsOptedOutPropagatesStoreError) {
    store_->ForceStoreError(true);
    auto mgr = MakeManager(store_, logger_);
    auto r = mgr.is_session_opted_out(kValidSession);
    EXPECT_FALSE(r.ok());
}

// ============ End-to-end through the real MemoryStore adapter =============
// Proves the §4.1/§4.2 migration (pure ADD COLUMN + partial index) + the three
// MemoryStore opt-out methods + MemoryStoreOptOutAdapter all work on real SQLite.

class OptOutAdapterE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryStore>(null_store_);
        ASSERT_TRUE(store_->Init(":memory:").ok());
    }
    NullCortrixStore null_store_;
    std::unique_ptr<MemoryStore> store_;
};

TEST_F(OptOutAdapterE2ETest, MigrationIsPureAddSessionStillRoundTrips) {
    // Create a session via the frozen path — the new columns default to empty.
    MemorySession s;
    s.namespace_name = "ns_a";
    s.user_id = "user_1";
    ASSERT_TRUE(store_->SessionCreate(s).ok());
    ASSERT_FALSE(s.session_id.empty());

    MemorySession got;
    ASSERT_TRUE(store_->SessionGet(s.session_id, got).ok());
    EXPECT_EQ(got.namespace_name, "ns_a");   // frozen column intact
    EXPECT_EQ(got.user_id, "user_1");
    EXPECT_TRUE(got.opt_out_at.empty());      // new column, default NULL → empty
    EXPECT_TRUE(got.opted_out_by.empty());
}

TEST_F(OptOutAdapterE2ETest, AdapterOptOutRevokeCycleOverRealStore) {
    MemorySession s;
    s.namespace_name = "ns_a";
    ASSERT_TRUE(store_->SessionCreate(s).ok());

    MemoryStoreOptOutAdapter adapter(*store_);
    auto mgr = OptOutManager(
        std::shared_ptr<ISessionOptOutStore>(&adapter, [](ISessionOptOutStore*) {}),
        /*logger=*/nullptr, /*enabled=*/true);

    // initial active
    auto a = mgr.is_session_opted_out(s.session_id);
    ASSERT_TRUE(a.ok());
    EXPECT_FALSE(*a);

    // opt out → persisted to memory_sessions
    auto r = mgr.OptOut(s.session_id, OptOutActor::kTest, "dev session");
    ASSERT_TRUE(r.ok()) << r.status().message();
    MemorySession got;
    ASSERT_TRUE(store_->SessionGet(s.session_id, got).ok());
    EXPECT_FALSE(got.opt_out_at.empty());
    EXPECT_EQ(got.opted_out_by, "test");
    EXPECT_TRUE(*mgr.is_session_opted_out(s.session_id));

    // revoke → cleared
    ASSERT_TRUE(mgr.OptOutRevoke(s.session_id, "changed mind").ok());
    ASSERT_TRUE(store_->SessionGet(s.session_id, got).ok());
    EXPECT_TRUE(got.opt_out_at.empty());
    EXPECT_TRUE(got.opted_out_by.empty());
    EXPECT_FALSE(*mgr.is_session_opted_out(s.session_id));
}

TEST_F(OptOutAdapterE2ETest, AdapterGetOptOutStateAbsentSession) {
    MemoryStoreOptOutAdapter adapter(*store_);
    auto st = adapter.GetOptOutState("11111111-2222-4333-8444-555566667777");
    ASSERT_TRUE(st.ok());
    EXPECT_FALSE(st->exists);
    EXPECT_FALSE(st->opted_out);
}

TEST_F(OptOutAdapterE2ETest, StoreSetOptOutOnAbsentSessionIsNotFound) {
    auto s = store_->SessionSetOptOut("11111111-2222-4333-8444-555566667777",
                                      "2026-05-16T10:30:00.000Z", "test");
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

// SessionClearOptOut on a session that does not exist → changes()==0 → NotFound
// (memory_store.cpp line 855).
TEST_F(OptOutAdapterE2ETest, StoreClearOptOutOnAbsentSessionIsNotFound) {
    auto s = store_->SessionClearOptOut("11111111-2222-4333-8444-555566667777");
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

// Adapter ClearOptOut on an absent session propagates the store NotFound.
TEST_F(OptOutAdapterE2ETest, AdapterClearOptOutOnAbsentSessionIsNotFound) {
    MemoryStoreOptOutAdapter adapter(*store_);
    auto s = adapter.ClearOptOut("11111111-2222-4333-8444-555566667777");
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

// --- Injected store-failure seam through the REAL adapter (SetFailNextOps) ---
// Arming the underlying MemoryStore makes the adapter's store calls fail, so the
// adapter's `if (!s.ok()) return s` propagation (opt_out_manager.cpp L209) and the
// manager's store-error branches run against the real SQLite-backed path.

TEST_F(OptOutAdapterE2ETest, AdapterGetOptOutStatePropagatesStoreFailure) {
    MemoryStoreOptOutAdapter adapter(*store_);
    store_->SetFailNextOps(1);  // SessionGetOptOut will fail
    auto st = adapter.GetOptOutState("11111111-2222-4333-8444-555566667777");
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.status().code(), StatusCode::kInternal);
}

TEST_F(OptOutAdapterE2ETest, AdapterSetOptOutPropagatesStoreFailure) {
    MemoryStoreOptOutAdapter adapter(*store_);
    store_->SetFailNextOps(1);  // SessionSetOptOut will fail
    auto s = adapter.SetOptOut("11111111-2222-4333-8444-555566667777",
                               "2026-05-16T10:30:00.000Z", "test");
    EXPECT_EQ(s.code(), StatusCode::kInternal);
}

TEST_F(OptOutAdapterE2ETest, AdapterClearOptOutPropagatesStoreFailure) {
    MemoryStoreOptOutAdapter adapter(*store_);
    store_->SetFailNextOps(1);  // SessionClearOptOut will fail
    EXPECT_EQ(adapter.ClearOptOut("any").code(), StatusCode::kInternal);
}

// OptOutManager over the real adapter: a store failure during GetOptOutState
// propagates out of OptOut (manager L114-115) without any stamp.
TEST_F(OptOutAdapterE2ETest, ManagerOptOutPropagatesAdapterGetStateFailure) {
    MemorySession s;
    s.namespace_name = "ns_a";
    ASSERT_TRUE(store_->SessionCreate(s).ok());

    MemoryStoreOptOutAdapter adapter(*store_);
    auto mgr = OptOutManager(
        std::shared_ptr<ISessionOptOutStore>(&adapter, [](ISessionOptOutStore*) {}),
        /*logger=*/nullptr, /*enabled=*/true);

    store_->SetFailNextOps(1);  // GetOptOutState (first store op in OptOut) fails
    auto r = mgr.OptOut(s.session_id, OptOutActor::kTest);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
}

}  // namespace
}  // namespace cortrix::memory::immunity
