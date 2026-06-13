#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/memory/mem03_metrics.h"
#include "cortrix/memory/memory_transparency.h"

// MEM03 §11.2 / §11.3 standalone integration: the full CRUD lifecycle over a single
// shared in-memory store + operation logger (the D3.5 real MemoryStore adapter / server
// routing / MEM02-MEM05 wiring are deferred). Covers End2End (list→create→edit→delete→
// list), the phased rollout, the F18a operation_log integration across endpoints, the
// MEM05 multi-user isolation + 404 mask, the admin-revoke visibility (revoked_at), and
// the Agent self-management E2E.
namespace cortrix::memory::transparency {
namespace {

using observability::OperationLogEntry;

// A single shared store backs both the lister and the block store, so writes are
// immediately visible to subsequent list/get — like the real DB-backed adapters.
class IntegStore {
public:
    std::map<std::string, MemoryBlockRecord> blocks;
};

class StoreLister : public IMemoryBlockLister {
public:
    explicit StoreLister(std::shared_ptr<IntegStore> s) : s_(std::move(s)) {}
    Result<std::vector<MemoryBlockRecord>> ListByUser(const std::string& user_id) override {
        std::vector<MemoryBlockRecord> out;
        for (const auto& [id, rec] : s_->blocks) {
            const bool is_memory = rec.metadata_json.is_object() &&
                                   rec.metadata_json.contains("memory_type");
            const std::string owner =
                !rec.user_id.empty() ? rec.user_id
                                     : (is_memory && rec.metadata_json.contains("user_id") &&
                                                rec.metadata_json["user_id"].is_string()
                                            ? rec.metadata_json["user_id"].get<std::string>()
                                            : std::string());
            if (is_memory && owner == user_id) out.push_back(rec);
        }
        return out;
    }

private:
    std::shared_ptr<IntegStore> s_;
};

class StoreBlock : public IMemoryBlockStore {
public:
    explicit StoreBlock(std::shared_ptr<IntegStore> s) : s_(std::move(s)) {}
    Result<std::string> InsertMemoryBlock(const MemoryBlockRecord& b) override {
        s_->blocks[b.block_id] = b;
        return b.block_id;
    }
    Status UpdateMemoryBlock(const MemoryBlockRecord& b) override {
        if (!s_->blocks.count(b.block_id)) return Status::NotFound(b.block_id);
        s_->blocks[b.block_id] = b;
        return Status::Ok();
    }
    Result<MemoryBlockRecord> GetMemoryBlock(const std::string& id) override {
        auto it = s_->blocks.find(id);
        if (it == s_->blocks.end()) return Status::NotFound(id);
        return it->second;
    }

private:
    std::shared_ptr<IntegStore> s_;
};

class RecordingOpLogger : public observability::IOperationLogger {
public:
    void Log(const OperationLogEntry& e, const observability::TraceContext* = nullptr) override {
        logged.push_back(e);
    }
    void BatchLog(const std::vector<OperationLogEntry>& es,
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
        int n = 0;
        for (const auto& e : logged) if (e.action == a) ++n;
        return n;
    }
    std::vector<OperationLogEntry> logged;
};

class MemoryTransparencyIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        Mem03Metrics::Instance().ResetForTest();
        store_ = std::make_shared<IntegStore>();
        oplog_ = std::make_shared<RecordingOpLogger>();
        mt_ = std::make_unique<MemoryTransparency>(
            std::make_shared<StoreLister>(store_),
            std::make_shared<StoreBlock>(store_), oplog_);
    }
    void TearDown() override { Mem03Metrics::Instance().ResetForTest(); }

    MemoryListFilter Filter(const std::string& user, bool include_invalidated = false) {
        MemoryListFilter f;
        f.user_id = user;
        f.include_invalidated = include_invalidated;
        return f;
    }

    std::shared_ptr<IntegStore> store_;
    std::shared_ptr<RecordingOpLogger> oplog_;
    std::unique_ptr<MemoryTransparency> mt_;
    static constexpr const char* kUser = "user_xxx";
};

// §11.2 MemoryTransparency_End2End: list → create → edit → delete → list.
TEST_F(MemoryTransparencyIntegrationTest, End2End) {
    // Start empty.
    auto l0 = mt_->List(Filter(kUser), kUser);
    ASSERT_TRUE(l0.ok());
    EXPECT_EQ(l0.value().total, 0);

    // Create.
    MemoryCreateRequest cr;
    cr.user_id = kUser;
    cr.content = "user uses Python";
    cr.memory_type = "fact";
    auto c = mt_->Create(cr, kUser);
    ASSERT_TRUE(c.ok());
    const std::string id1 = c.value();

    auto l1 = mt_->List(Filter(kUser), kUser);
    ASSERT_TRUE(l1.ok());
    EXPECT_EQ(l1.value().total, 1);

    // Edit (issue-2 B: new block + invalidate old).
    MemoryEditRequest er;
    er.memory_id = id1;
    er.new_content = "user switched to Go";
    auto e = mt_->Edit(er, kUser);
    ASSERT_TRUE(e.ok());
    const std::string id2 = e.value().new_memory_id;
    EXPECT_EQ(e.value().old_memory_id, id1);

    // After edit: default list shows only the new (active) block; full history shows both.
    auto l2 = mt_->List(Filter(kUser), kUser);
    ASSERT_TRUE(l2.ok());
    EXPECT_EQ(l2.value().total, 1);
    EXPECT_EQ(l2.value().memories[0].memory_id, id2);

    auto l2_all = mt_->List(Filter(kUser, /*include_invalidated=*/true), kUser);
    ASSERT_TRUE(l2_all.ok());
    EXPECT_EQ(l2_all.value().total, 2);

    // Delete the new block (soft).
    Status d = mt_->Delete(id2, kUser);
    ASSERT_TRUE(d.ok());

    // Default list now empty; full history still 2.
    auto l3 = mt_->List(Filter(kUser), kUser);
    ASSERT_TRUE(l3.ok());
    EXPECT_EQ(l3.value().total, 0);
    auto l3_all = mt_->List(Filter(kUser, true), kUser);
    ASSERT_TRUE(l3_all.ok());
    EXPECT_EQ(l3_all.value().total, 2);
}

// §11.2 MemoryTransparency_PhasedRollout: default A-class only; explain adds B-class.
TEST_F(MemoryTransparencyIntegrationTest, PhasedRollout) {
    MemoryCreateRequest cr;
    cr.user_id = kUser;
    cr.content = "c";
    cr.memory_type = "fact";
    ASSERT_TRUE(mt_->Create(cr, kUser).ok());

    auto def = mt_->List(Filter(kUser), kUser, /*explain=*/false);
    ASSERT_TRUE(def.ok());
    ASSERT_EQ(def.value().memories.size(), 1u);
    EXPECT_FALSE(def.value().memories[0].extraction_method.has_value());

    auto ex = mt_->List(Filter(kUser), kUser, /*explain=*/true);
    ASSERT_TRUE(ex.ok());
    ASSERT_EQ(ex.value().memories.size(), 1u);
    ASSERT_TRUE(ex.value().memories[0].extraction_method.has_value());
    EXPECT_EQ(*ex.value().memories[0].extraction_method, "explicit");  // create path
}

// §11.2 MemoryTransparency_F18aOpLogIntegration: create/edit/delete all emit audit.
TEST_F(MemoryTransparencyIntegrationTest, F18aOpLogIntegration) {
    MemoryCreateRequest cr;
    cr.user_id = kUser;
    cr.content = "c";
    cr.memory_type = "fact";
    const std::string id1 = mt_->Create(cr, kUser).value();

    MemoryEditRequest er;
    er.memory_id = id1;
    er.new_content = "c2";
    const std::string id2 = mt_->Edit(er, kUser).value().new_memory_id;

    ASSERT_TRUE(mt_->Delete(id2, kUser).ok());

    // create=1, edit=1, invalidate=2 (edit cascade + delete).
    EXPECT_EQ(oplog_->CountAction("memory_create"), 1);
    EXPECT_EQ(oplog_->CountAction("memory_edit"), 1);
    EXPECT_EQ(oplog_->CountAction("memory_invalidate"), 2);

    // All audit records target resource_type=memory + carry triggered_by in summary.
    for (const auto& e : oplog_->logged) {
        EXPECT_EQ(e.resource_type, "memory");
        ASSERT_TRUE(e.summary.has_value());
        EXPECT_NE(e.summary->find("triggered_by="), std::string::npos);
    }
}

// §11.2 MemoryTransparency_MEM05Isolation: multi-user isolation + 404 mask.
TEST_F(MemoryTransparencyIntegrationTest, MEM05Isolation) {
    // user_a and user_b each create a memory.
    MemoryCreateRequest ca;
    ca.user_id = "user_a";
    ca.content = "a's secret";
    ca.memory_type = "fact";
    const std::string id_a = mt_->Create(ca, "user_a").value();

    MemoryCreateRequest cb;
    cb.user_id = "user_b";
    cb.content = "b's secret";
    cb.memory_type = "fact";
    ASSERT_TRUE(mt_->Create(cb, "user_b").ok());

    // user_a lists only their own.
    auto la = mt_->List(Filter("user_a"), "user_a");
    ASSERT_TRUE(la.ok());
    ASSERT_EQ(la.value().memories.size(), 1u);
    EXPECT_EQ(la.value().memories[0].content, "a's secret");

    // user_b cannot edit/delete user_a's block — masked as 404 (not 403-leaks-existence).
    MemoryEditRequest er;
    er.memory_id = id_a;
    er.new_content = "hijack";
    auto e = mt_->Edit(er, "user_b");
    ASSERT_FALSE(e.ok());
    EXPECT_EQ(e.status().code(), StatusCode::kNotFound);

    Status d = mt_->Delete(id_a, "user_b");
    ASSERT_FALSE(d.ok());
    EXPECT_EQ(d.code(), StatusCode::kNotFound);

    // The cross-user attempts were counted (2: one edit + one delete).
    EXPECT_EQ(Mem03Metrics::Instance().CrossUserBlockedCount(), 2u);
    // user_a's block is intact + active.
    EXPECT_EQ(store_->blocks[id_a].metadata_json["status"], "active");
    EXPECT_EQ(store_->blocks[id_a].content, "a's secret");
}

// §11.2 MemoryTransparency_AdminRevokeVisible: after a (simulated MEM02 admin) revoke
// stamps revoked_at + restores status=active, the user's list surfaces revoked_at
// (the A-class transparency field). The admin revoke path itself is MEM02's; here we
// simulate its metadata_json effect on the shared store and assert MEM03 surfaces it.
TEST_F(MemoryTransparencyIntegrationTest, AdminRevokeVisible) {
    // Seed an LLM-invalidated block, then simulate admin revoke (status→active +
    // revoked_at set), exactly as MEM02 AdminRevokeInvalidate writes to metadata_json.
    MemoryBlockRecord rec;
    rec.block_id = "mem_001";
    rec.user_id = kUser;
    rec.content = "user switched to Go";
    rec.metadata_json = {
        {"memory_type", "fact"},
        {"status", "active"},          // restored by admin revoke
        {"user_id", kUser},
        {"extraction_method", "llm"},
        {"invalidated_by_block_id", nullptr},
        {"invalidated_at", nullptr},
        {"revoked_at", static_cast<int64_t>(1716220800000)},  // admin revoke time
    };
    store_->blocks["mem_001"] = rec;

    // Default list (active) surfaces revoked_at in A-class.
    auto l = mt_->List(Filter(kUser), kUser);
    ASSERT_TRUE(l.ok());
    ASSERT_EQ(l.value().memories.size(), 1u);
    const auto& m = l.value().memories[0];
    EXPECT_EQ(m.memory_id, "mem_001");
    EXPECT_EQ(m.status, "active");
    ASSERT_TRUE(m.revoked_at.has_value());
    EXPECT_EQ(*m.revoked_at, 1716220800000LL);
}

// §11.3 MemoryTransparency_AgentSelfManagement: Agent self-service list → create →
// edit → delete closed loop (P14 SDK is mocked at the service boundary).
TEST_F(MemoryTransparencyIntegrationTest, AgentSelfManagement) {
    // Agent creates three memories.
    for (int i = 0; i < 3; ++i) {
        MemoryCreateRequest cr;
        cr.user_id = kUser;
        cr.content = "fact " + std::to_string(i);
        cr.memory_type = "fact";
        ASSERT_TRUE(mt_->Create(cr, kUser).ok());
    }
    auto l = mt_->List(Filter(kUser), kUser);
    ASSERT_TRUE(l.ok());
    EXPECT_EQ(l.value().total, 3);

    // Agent edits one (pick the first listed) and deletes another.
    const std::string edit_id = l.value().memories[0].memory_id;
    const std::string del_id = l.value().memories[1].memory_id;
    MemoryEditRequest er;
    er.memory_id = edit_id;
    er.new_content = "updated fact";
    ASSERT_TRUE(mt_->Edit(er, kUser).ok());
    ASSERT_TRUE(mt_->Delete(del_id, kUser).ok());

    // Net effective memories: 3 - 1 deleted - 1 edited-old + 1 edited-new = 2 active.
    auto l2 = mt_->List(Filter(kUser), kUser);
    ASSERT_TRUE(l2.ok());
    EXPECT_EQ(l2.value().total, 2);

    // The op metrics reflect the full self-service loop.
    auto& metrics = Mem03Metrics::Instance();
    EXPECT_EQ(metrics.OpCount(Mem03Metrics::Op::kCreate, Mem03Metrics::OpStatus::kSuccess), 3u);
    EXPECT_EQ(metrics.OpCount(Mem03Metrics::Op::kEdit, Mem03Metrics::OpStatus::kSuccess), 1u);
    EXPECT_EQ(metrics.OpCount(Mem03Metrics::Op::kInvalidate, Mem03Metrics::OpStatus::kSuccess), 1u);
    // List is called twice in this loop (after the creates, then at the end).
    EXPECT_EQ(metrics.OpCount(Mem03Metrics::Op::kList, Mem03Metrics::OpStatus::kSuccess), 2u);
}

}  // namespace
}  // namespace cortrix::memory::transparency
