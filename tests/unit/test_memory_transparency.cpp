#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/memory/mem03_error.h"
#include "cortrix/memory/mem03_metrics.h"
#include "cortrix/memory/memory_transparency.h"

// S1-S5 coverage: the MemoryTransparency service against injected seams (a list-by-user
// seam, the memory extraction IMemoryBlockStore, and the operation logger). Covers memory transparency
// — user-isolation List, include_invalidated, issue-2 B edit (new+invalidate old),
// optimistic lock, cross-user 404 mask, soft delete + idempotence, create validation,
// no-LLM create, the phased-rollout A/B projection, and the operation_log hooks.
namespace cortrix::memory::transparency {
namespace {

using observability::OperationLogEntry;

// --- Fake list seam: returns the blocks owned by a user from an in-memory store. ---
// Backed by the same map the FakeBlockStore uses, so a created/edited block is
// immediately listable (a single shared store, like the real DB).
class SharedStore {
public:
    void Put(const MemoryBlockRecord& rec) { blocks_[rec.block_id] = rec; }
    const MemoryBlockRecord* Get(const std::string& id) const {
        auto it = blocks_.find(id);
        return it == blocks_.end() ? nullptr : &it->second;
    }
    std::map<std::string, MemoryBlockRecord> blocks_;
};

class FakeBlockLister : public IMemoryBlockLister {
public:
    explicit FakeBlockLister(std::shared_ptr<SharedStore> store) : store_(std::move(store)) {}
    Result<std::vector<MemoryBlockRecord>> ListByUser(const std::string& user_id) override {
        if (fail_) return Status::Internal("list backend down");
        std::vector<MemoryBlockRecord> out;
        for (const auto& [id, rec] : store_->blocks_) {
            const std::string owner =
                !rec.user_id.empty() ? rec.user_id
                                     : (rec.metadata_json.is_object() &&
                                                rec.metadata_json.contains("user_id") &&
                                                rec.metadata_json["user_id"].is_string()
                                            ? rec.metadata_json["user_id"].get<std::string>()
                                            : std::string());
            // A memory block = has a non-empty memory_type in metadata_json.
            const bool is_memory = rec.metadata_json.is_object() &&
                                   rec.metadata_json.contains("memory_type");
            if (owner == user_id && is_memory) out.push_back(rec);
        }
        return out;
    }
    void SetFail(bool f) { fail_ = f; }

private:
    std::shared_ptr<SharedStore> store_;
    bool fail_ = false;
};

// --- Fake block store over the shared map (mirrors test_memory_extractor FakeBlockStore). ---
class FakeBlockStore : public IMemoryBlockStore {
public:
    explicit FakeBlockStore(std::shared_ptr<SharedStore> store) : store_(std::move(store)) {}
    Result<std::string> InsertMemoryBlock(const MemoryBlockRecord& block) override {
        if (insert_fail_) return Status::Internal("insert failed");
        store_->blocks_[block.block_id] = block;
        inserted.push_back(block.block_id);
        return block.block_id;
    }
    Status UpdateMemoryBlock(const MemoryBlockRecord& block) override {
        if (update_fail_) return Status::Internal("update failed");
        if (!store_->blocks_.count(block.block_id)) {
            return Status::NotFound("no such block: " + block.block_id);
        }
        store_->blocks_[block.block_id] = block;
        return Status::Ok();
    }
    Result<MemoryBlockRecord> GetMemoryBlock(const std::string& block_id) override {
        auto it = store_->blocks_.find(block_id);
        if (it == store_->blocks_.end()) return Status::NotFound("no such block: " + block_id);
        return it->second;
    }
    void SetInsertFail(bool f) { insert_fail_ = f; }
    void SetUpdateFail(bool f) { update_fail_ = f; }
    std::vector<std::string> inserted;

private:
    std::shared_ptr<SharedStore> store_;
    bool insert_fail_ = false;
    bool update_fail_ = false;
};

class CountingOpLogger : public observability::IOperationLogger {
public:
    void Log(const OperationLogEntry& entry,
             const observability::TraceContext* ctx = nullptr) override {
        (void)ctx;
        logged.push_back(entry);
    }
    void BatchLog(const std::vector<OperationLogEntry>& entries,
                  const observability::TraceContext* ctx = nullptr) override {
        (void)ctx;
        for (const auto& e : entries) logged.push_back(e);
    }
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext* = nullptr) override {
        return observability::OperationLogQueryResult{};
    }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }

    int CountAction(const std::string& action) const {
        int n = 0;
        for (const auto& e : logged) if (e.action == action) ++n;
        return n;
    }
    const OperationLogEntry* LastWithAction(const std::string& action) const {
        for (auto it = logged.rbegin(); it != logged.rend(); ++it)
            if (it->action == action) return &*it;
        return nullptr;
    }
    std::vector<OperationLogEntry> logged;
};

// Seed a memory block directly into the shared store.
MemoryBlockRecord MakeMemory(const std::string& id, const std::string& user,
                             const std::string& content, const std::string& type,
                             const std::string& status, int64_t last_modified = 1000) {
    MemoryBlockRecord rec;
    rec.block_id = id;
    rec.user_id = user;
    rec.content = content;
    rec.metadata_json = {
        {"memory_type", type},
        {"status", status},
        {"user_id", user},
        {"extraction_method", "llm"},
        {"extracted_at", static_cast<int64_t>(500)},
        {"last_modified_at", last_modified},
    };
    return rec;
}

class MemoryTransparencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        Mem03Metrics::Instance().ResetForTest();
        store_ = std::make_shared<SharedStore>();
        lister_ = std::make_shared<FakeBlockLister>(store_);
        block_store_ = std::make_shared<FakeBlockStore>(store_);
        oplog_ = std::make_shared<CountingOpLogger>();
        mt_ = std::make_unique<MemoryTransparency>(lister_, block_store_, oplog_);
    }
    void TearDown() override { Mem03Metrics::Instance().ResetForTest(); }

    std::shared_ptr<SharedStore> store_;
    std::shared_ptr<FakeBlockLister> lister_;
    std::shared_ptr<FakeBlockStore> block_store_;
    std::shared_ptr<CountingOpLogger> oplog_;
    std::unique_ptr<MemoryTransparency> mt_;

    static constexpr const char* kUser = "user_xxx";
};

// ---------- List ----------

TEST_F(MemoryTransparencyTest, ListUserIdMustMatchSession) {
    MemoryListFilter f;
    f.user_id = "other_user";
    auto r = mt_->List(f, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kPermissionDenied);
    EXPECT_NE(r.status().message().find("CX_ERR_MEM03_USER_MISMATCH"), std::string::npos);
    EXPECT_EQ(Mem03Metrics::Instance().OpCount(Mem03Metrics::Op::kList,
                                               Mem03Metrics::OpStatus::kError), 1u);
}

TEST_F(MemoryTransparencyTest, ListIncludeInvalidatedFalseHidesInvalidated) {
    store_->Put(MakeMemory("m1", kUser, "active fact", "fact", "active"));
    store_->Put(MakeMemory("m2", kUser, "old fact", "fact", "invalidated"));
    store_->Put(MakeMemory("m3", kUser, "tentative", "preference", "tentative"));
    MemoryListFilter f;
    f.user_id = kUser;
    auto r = mt_->List(f, kUser);
    ASSERT_TRUE(r.ok());
    // active + tentative kept; invalidated hidden.
    EXPECT_EQ(r.value().total, 2);
    EXPECT_EQ(r.value().memories.size(), 2u);
    for (const auto& m : r.value().memories) EXPECT_NE(m.status, "invalidated");
}

TEST_F(MemoryTransparencyTest, ListIncludeInvalidatedTrueReturnsFullHistory) {
    store_->Put(MakeMemory("m1", kUser, "active", "fact", "active"));
    store_->Put(MakeMemory("m2", kUser, "old", "fact", "invalidated"));
    MemoryListFilter f;
    f.user_id = kUser;
    f.include_invalidated = true;
    auto r = mt_->List(f, kUser);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total, 2);
}

TEST_F(MemoryTransparencyTest, ListFiltersByMemoryType) {
    store_->Put(MakeMemory("m1", kUser, "a fact", "fact", "active"));
    store_->Put(MakeMemory("m2", kUser, "a pref", "preference", "active"));
    MemoryListFilter f;
    f.user_id = kUser;
    f.memory_type = "preference";
    auto r = mt_->List(f, kUser);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().memories.size(), 1u);
    EXPECT_EQ(r.value().memories[0].memory_type, "preference");
}

TEST_F(MemoryTransparencyTest, ListOnlyReturnsSessionUsersMemories) {
    store_->Put(MakeMemory("m1", kUser, "mine", "fact", "active"));
    store_->Put(MakeMemory("m2", "other_user", "theirs", "fact", "active"));
    MemoryListFilter f;
    f.user_id = kUser;
    auto r = mt_->List(f, kUser);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().memories.size(), 1u);
    EXPECT_EQ(r.value().memories[0].memory_id, "m1");
}

TEST_F(MemoryTransparencyTest, ListPaginationClampsPageSize) {
    for (int i = 0; i < 5; ++i)
        store_->Put(MakeMemory("m" + std::to_string(i), kUser, "c", "fact", "active"));
    MemoryListFilter f;
    f.user_id = kUser;
    f.page = 0;
    f.page_size = 1000;  // clamped to 200
    auto r = mt_->List(f, kUser);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().page_size, kMaxPageSize);
    EXPECT_EQ(r.value().total, 5);
    EXPECT_EQ(r.value().memories.size(), 5u);  // all fit in one (clamped) page
    ASSERT_FALSE(r.value().warnings.empty());
}

TEST_F(MemoryTransparencyTest, ListPaginationSecondPage) {
    for (int i = 0; i < 5; ++i)
        store_->Put(MakeMemory("m" + std::to_string(i), kUser, "c", "fact", "active"));
    MemoryListFilter f;
    f.user_id = kUser;
    f.page = 1;
    f.page_size = 2;
    auto r = mt_->List(f, kUser);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total, 5);            // total is the full matched count
    EXPECT_EQ(r.value().memories.size(), 2u);  // page 1 of size 2 = items [2,3]
}

TEST_F(MemoryTransparencyTest, ListPhasedRolloutDefaultHidesBClass) {
    store_->Put(MakeMemory("m1", kUser, "c", "fact", "active"));
    MemoryListFilter f;
    f.user_id = kUser;
    auto r = mt_->List(f, kUser, /*explain=*/false);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().memories.size(), 1u);
    // A-class present, B-class absent.
    EXPECT_EQ(r.value().memories[0].content, "c");
    EXPECT_FALSE(r.value().memories[0].extraction_method.has_value());
    EXPECT_FALSE(r.value().memories[0].source_session_id.has_value());
}

TEST_F(MemoryTransparencyTest, ListPhasedRolloutExplainShowsBClass) {
    MemoryBlockRecord rec = MakeMemory("m1", kUser, "c", "fact", "active");
    rec.metadata_json["source_session_id"] = "sess_abc";
    rec.metadata_json["source_interaction_id"] = "int_def";
    store_->Put(rec);
    MemoryListFilter f;
    f.user_id = kUser;
    auto r = mt_->List(f, kUser, /*explain=*/true);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().memories.size(), 1u);
    const auto& m = r.value().memories[0];
    ASSERT_TRUE(m.extraction_method.has_value());
    EXPECT_EQ(*m.extraction_method, "llm");
    ASSERT_TRUE(m.source_session_id.has_value());
    EXPECT_EQ(*m.source_session_id, "sess_abc");
    EXPECT_EQ(*m.source_interaction_id, "int_def");
}

TEST_F(MemoryTransparencyTest, ListBackendErrorPropagates) {
    lister_->SetFail(true);
    MemoryListFilter f;
    f.user_id = kUser;
    auto r = mt_->List(f, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
}

// ---------- Create ----------

TEST_F(MemoryTransparencyTest, CreateHappyPathNoLlm) {
    MemoryCreateRequest req;
    req.user_id = kUser;
    req.content = "user switched to Go";
    req.memory_type = "fact";
    auto r = mt_->Create(req, kUser);
    ASSERT_TRUE(r.ok());
    const std::string id = r.value();
    EXPECT_FALSE(id.empty());
    // Stored with extraction_method=explicit (issue-3 A: no LLM path).
    const MemoryBlockRecord* rec = store_->Get(id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->metadata_json["extraction_method"], "explicit");
    EXPECT_EQ(rec->metadata_json["status"], "active");
    EXPECT_EQ(rec->metadata_json["memory_type"], "fact");
    // memory_create audit emitted.
    EXPECT_EQ(oplog_->CountAction("memory_create"), 1);
}

TEST_F(MemoryTransparencyTest, CreateUserMismatchRejected) {
    MemoryCreateRequest req;
    req.user_id = "other_user";
    req.content = "x";
    req.memory_type = "fact";
    auto r = mt_->Create(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kPermissionDenied);
}

TEST_F(MemoryTransparencyTest, CreateInvalidType) {
    MemoryCreateRequest req;
    req.user_id = kUser;
    req.content = "x";
    req.memory_type = "bogus";
    auto r = mt_->Create(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(r.status().message().find("CX_ERR_MEM03_INVALID_TYPE"), std::string::npos);
}

TEST_F(MemoryTransparencyTest, CreateContentTooLong) {
    MemoryCreateRequest req;
    req.user_id = kUser;
    req.content = std::string(kMaxContentLength + 1, 'a');
    req.memory_type = "fact";
    auto r = mt_->Create(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(r.status().message().find("CX_ERR_MEM03_CONTENT_TOO_LONG"), std::string::npos);
}

TEST_F(MemoryTransparencyTest, CreateContentExactlyMaxAllowed) {
    MemoryCreateRequest req;
    req.user_id = kUser;
    req.content = std::string(kMaxContentLength, 'a');  // boundary: 2000 is allowed
    req.memory_type = "event";
    auto r = mt_->Create(req, kUser);
    EXPECT_TRUE(r.ok());
}

TEST_F(MemoryTransparencyTest, CreateInsertFailurePropagates) {
    block_store_->SetInsertFail(true);
    MemoryCreateRequest req;
    req.user_id = kUser;
    req.content = "x";
    req.memory_type = "fact";
    auto r = mt_->Create(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(oplog_->CountAction("memory_create"), 0);  // no audit on failure
}

// ---------- Edit (issue-2 B) ----------

TEST_F(MemoryTransparencyTest, EditNewBlockInvalidatesOld) {
    store_->Put(MakeMemory("m_old", kUser, "user uses TensorFlow", "preference", "active"));
    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "user prefers PyTorch";
    auto r = mt_->Edit(req, kUser);
    ASSERT_TRUE(r.ok());
    const std::string new_id = r.value().new_memory_id;
    EXPECT_EQ(r.value().old_memory_id, "m_old");
    EXPECT_NE(new_id, "m_old");
    // New block: active, user_edit, new content.
    const MemoryBlockRecord* nb = store_->Get(new_id);
    ASSERT_NE(nb, nullptr);
    EXPECT_EQ(nb->content, "user prefers PyTorch");
    EXPECT_EQ(nb->metadata_json["extraction_method"], "user_edit");
    EXPECT_EQ(nb->metadata_json["status"], "active");
    // Old block: invalidated, invalidated_by_block_id = new.
    const MemoryBlockRecord* ob = store_->Get("m_old");
    ASSERT_NE(ob, nullptr);
    EXPECT_EQ(ob->metadata_json["status"], "invalidated");
    EXPECT_EQ(ob->metadata_json["invalidated_by_block_id"], new_id);
    // Two audit records: memory_edit + cascaded memory_invalidate.
    EXPECT_EQ(oplog_->CountAction("memory_edit"), 1);
    EXPECT_EQ(oplog_->CountAction("memory_invalidate"), 1);
    const auto* inv = oplog_->LastWithAction("memory_invalidate");
    ASSERT_NE(inv, nullptr);
    ASSERT_TRUE(inv->summary.has_value());
    EXPECT_NE(inv->summary->find("triggered_by=user_edit"), std::string::npos);
}

TEST_F(MemoryTransparencyTest, EditOptimisticLockConflict) {
    store_->Put(MakeMemory("m_old", kUser, "c", "fact", "active", /*last_modified=*/1234));
    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "c2";
    req.expected_modified_at = 9999;  // stale — does not match 1234
    auto r = mt_->Edit(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM03_EDIT_CONCURRENT"), std::string::npos);
    EXPECT_EQ(Mem03Metrics::Instance().EditConflictCount(), 1u);
    // Old block untouched (still active, no new block inserted beyond none).
    EXPECT_EQ(store_->Get("m_old")->metadata_json["status"], "active");
}

TEST_F(MemoryTransparencyTest, EditOptimisticLockMatchSucceeds) {
    store_->Put(MakeMemory("m_old", kUser, "c", "fact", "active", /*last_modified=*/1234));
    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "c2";
    req.expected_modified_at = 1234;  // matches
    auto r = mt_->Edit(req, kUser);
    EXPECT_TRUE(r.ok());
}

TEST_F(MemoryTransparencyTest, EditCrossUserReturns404Mask) {
    store_->Put(MakeMemory("m_old", "user_xxx", "c", "fact", "active"));
    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "c2";
    auto r = mt_->Edit(req, "evil_user");  // attacker session
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
    EXPECT_NE(r.status().message().find("CX_ERR_MEM03_MEMORY_NOT_FOUND"), std::string::npos);
    EXPECT_EQ(Mem03Metrics::Instance().CrossUserBlockedCount(), 1u);
    // Original block untouched.
    EXPECT_EQ(store_->Get("m_old")->metadata_json["status"], "active");
}

TEST_F(MemoryTransparencyTest, EditAbsentReturns404) {
    MemoryEditRequest req;
    req.memory_id = "nope";
    req.new_content = "x";
    auto r = mt_->Edit(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
    // Absent (not cross-user) — masked as NOT_FOUND but NOT counted as cross-user block.
    EXPECT_EQ(Mem03Metrics::Instance().CrossUserBlockedCount(), 0u);
}

TEST_F(MemoryTransparencyTest, EditInvalidNewType) {
    store_->Put(MakeMemory("m_old", kUser, "c", "fact", "active"));
    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_memory_type = "bogus";
    auto r = mt_->Edit(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM03_INVALID_TYPE"), std::string::npos);
}

TEST_F(MemoryTransparencyTest, EditContentOnlyKeepsType) {
    store_->Put(MakeMemory("m_old", kUser, "c", "preference", "active"));
    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "c2";  // type omitted → inherited
    auto r = mt_->Edit(req, kUser);
    ASSERT_TRUE(r.ok());
    const MemoryBlockRecord* nb = store_->Get(r.value().new_memory_id);
    ASSERT_NE(nb, nullptr);
    EXPECT_EQ(nb->metadata_json["memory_type"], "preference");
}

TEST_F(MemoryTransparencyTest, EditInsertFailureRollsBackToError) {
    store_->Put(MakeMemory("m_old", kUser, "c", "fact", "active"));
    block_store_->SetInsertFail(true);
    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "c2";
    auto r = mt_->Edit(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM03_INVALIDATE_FAILED"), std::string::npos);
    // Old block NOT invalidated since the new insert failed first.
    EXPECT_EQ(store_->Get("m_old")->metadata_json["status"], "active");
}

// Edit carries source_session_id / source_interaction_id forward onto the new block
// so the lineage stays linkable in explain (lines 392/394).
TEST_F(MemoryTransparencyTest, EditCarriesSourceProvenanceForward) {
    MemoryBlockRecord old_rec = MakeMemory("m_old", kUser, "c", "fact", "active");
    old_rec.metadata_json["source_session_id"] = "sess_src";
    old_rec.metadata_json["source_interaction_id"] = "int_src";
    store_->Put(old_rec);

    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "c2";
    auto r = mt_->Edit(req, kUser);
    ASSERT_TRUE(r.ok());
    const MemoryBlockRecord* nb = store_->Get(r.value().new_memory_id);
    ASSERT_NE(nb, nullptr);
    EXPECT_EQ(nb->metadata_json["source_session_id"], "sess_src");
    EXPECT_EQ(nb->metadata_json["source_interaction_id"], "int_src");
}

// Edit: optimistic lock where the stored block has NO last_modified_at — the
// expected value cannot match (!cur.has_value() branch, line 364) → conflict.
TEST_F(MemoryTransparencyTest, EditOptimisticLockNoStoredModifiedAtConflicts) {
    MemoryBlockRecord rec;
    rec.block_id = "m_old";
    rec.user_id = kUser;
    rec.content = "c";
    rec.metadata_json = {{"memory_type", "fact"}, {"status", "active"},
                         {"user_id", kUser}};  // deliberately no last_modified_at
    store_->Put(rec);

    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "c2";
    req.expected_modified_at = 1234;  // nothing to match against
    auto r = mt_->Edit(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM03_EDIT_CONCURRENT"), std::string::npos);
    EXPECT_NE(r.status().message().find("none"), std::string::npos);  // actual=none
}

// Edit: the new block inserts fine but the old-block UPDATE (invalidation) fails —
// the INVALIDATE_FAILED path (lines 412-416). The fake only fails UPDATE, not INSERT.
TEST_F(MemoryTransparencyTest, EditOldBlockUpdateFailureReturnsInvalidateFailed) {
    store_->Put(MakeMemory("m_old", kUser, "c", "fact", "active"));
    block_store_->SetUpdateFail(true);  // INSERT still succeeds, UPDATE fails
    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "c2";
    auto r = mt_->Edit(req, kUser);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEM03_INVALIDATE_FAILED"), std::string::npos);
    EXPECT_EQ(Mem03Metrics::Instance().OpCount(Mem03Metrics::Op::kEdit,
                                               Mem03Metrics::OpStatus::kError), 1u);
}

// ---------- Delete (soft) ----------

TEST_F(MemoryTransparencyTest, DeleteSoftMarksInvalidated) {
    store_->Put(MakeMemory("m1", kUser, "c", "fact", "active"));
    Status s = mt_->Delete("m1", kUser);
    ASSERT_TRUE(s.ok());
    const MemoryBlockRecord* rec = store_->Get("m1");
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->metadata_json["status"], "invalidated");
    EXPECT_EQ(rec->metadata_json["deleted_by_user_id"], kUser);
    ASSERT_TRUE(rec->metadata_json.contains("deleted_at"));
    EXPECT_TRUE(rec->metadata_json["deleted_at"].is_number());
    // memory_invalidate audit with triggered_by=user_manual.
    EXPECT_EQ(oplog_->CountAction("memory_invalidate"), 1);
    const auto* inv = oplog_->LastWithAction("memory_invalidate");
    ASSERT_NE(inv, nullptr);
    ASSERT_TRUE(inv->summary.has_value());
    EXPECT_NE(inv->summary->find("triggered_by=user_manual"), std::string::npos);
    EXPECT_EQ(inv->resource_type, "memory");
}

TEST_F(MemoryTransparencyTest, DeleteAlreadyInvalidatedIsIdempotent) {
    store_->Put(MakeMemory("m1", kUser, "c", "fact", "invalidated"));
    Status s = mt_->Delete("m1", kUser);
    EXPECT_TRUE(s.ok());  // 200, no error
    // No new audit / no deleted_at stamped (short-circuit).
    EXPECT_EQ(oplog_->CountAction("memory_invalidate"), 0);
    EXPECT_FALSE(store_->Get("m1")->metadata_json.contains("deleted_at"));
}

TEST_F(MemoryTransparencyTest, DeleteCrossUserReturns404Mask) {
    store_->Put(MakeMemory("m1", "user_xxx", "c", "fact", "active"));
    Status s = mt_->Delete("m1", "evil_user");
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_NE(s.message().find("CX_ERR_MEM03_MEMORY_NOT_FOUND"), std::string::npos);
    EXPECT_EQ(Mem03Metrics::Instance().CrossUserBlockedCount(), 1u);
    EXPECT_EQ(store_->Get("m1")->metadata_json["status"], "active");  // untouched
}

TEST_F(MemoryTransparencyTest, DeleteAbsentReturns404) {
    Status s = mt_->Delete("nope", kUser);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST_F(MemoryTransparencyTest, DeleteUpdateFailurePropagates) {
    store_->Put(MakeMemory("m1", kUser, "c", "fact", "active"));
    block_store_->SetUpdateFail(true);
    Status s = mt_->Delete("m1", kUser);
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_MEM03_INVALIDATE_FAILED"), std::string::npos);
    EXPECT_EQ(oplog_->CountAction("memory_invalidate"), 0);  // no audit on failure
}

// ---------- helpers ----------

TEST_F(MemoryTransparencyTest, ProjectListItemMapsMetadata) {
    MemoryBlockRecord rec = MakeMemory("m1", kUser, "hello", "event", "active");
    rec.metadata_json["invalidated_by_block_id"] = "m0";
    rec.metadata_json["revoked_at"] = static_cast<int64_t>(1716220800000);
    MemoryListItem a = MemoryTransparency::ProjectListItem(rec, /*explain=*/false);
    EXPECT_EQ(a.memory_id, "m1");
    EXPECT_EQ(a.content, "hello");
    EXPECT_EQ(a.memory_type, "event");
    EXPECT_EQ(a.status, "active");
    EXPECT_EQ(a.user_id, kUser);
    ASSERT_TRUE(a.extracted_at.has_value());
    EXPECT_EQ(*a.extracted_at, 500);
    ASSERT_TRUE(a.revoked_at.has_value());
    EXPECT_EQ(*a.revoked_at, 1716220800000);
    EXPECT_FALSE(a.invalidated_by_block_id.has_value());  // B-class hidden

    MemoryListItem b = MemoryTransparency::ProjectListItem(rec, /*explain=*/true);
    ASSERT_TRUE(b.invalidated_by_block_id.has_value());
    EXPECT_EQ(*b.invalidated_by_block_id, "m0");
}

TEST_F(MemoryTransparencyTest, ProjectListItemParsesIsoTimestamp) {
    // Memory extraction stamps created_at as an ISO-8601 string; projection must coerce to epoch ms.
    MemoryBlockRecord rec;
    rec.block_id = "m1";
    rec.user_id = kUser;
    rec.content = "c";
    rec.metadata_json = {
        {"memory_type", "fact"},
        {"status", "active"},
        {"created_at", "2024-05-20T12:00:00Z"},  // ISO string, no numeric extracted_at
    };
    MemoryListItem a = MemoryTransparency::ProjectListItem(rec, false);
    ASSERT_TRUE(a.extracted_at.has_value());
    // 2024-05-20T12:00:00Z == 1716206400 s == 1716206400000 ms.
    EXPECT_EQ(*a.extracted_at, 1716206400000LL);
}

// ReadTimestampMs float branch (line 35): a floating-point extracted_at is
// truncated to int64 epoch ms.
TEST_F(MemoryTransparencyTest, ProjectListItemReadsFloatTimestamp) {
    MemoryBlockRecord rec;
    rec.block_id = "m1";
    rec.user_id = kUser;
    rec.content = "c";
    rec.metadata_json = {
        {"memory_type", "fact"},
        {"status", "active"},
        {"extracted_at", 1716206400500.0},  // float epoch ms
    };
    MemoryListItem a = MemoryTransparency::ProjectListItem(rec, false);
    ASSERT_TRUE(a.extracted_at.has_value());
    EXPECT_EQ(*a.extracted_at, 1716206400500LL);
}

// ReadTimestampMs: an unparseable ISO created_at (sscanf < 6 fields) yields nullopt,
// and with no numeric fallback extracted_at stays unset (line 41/57 path).
TEST_F(MemoryTransparencyTest, ProjectListItemMalformedIsoTimestampIsUnset) {
    MemoryBlockRecord rec;
    rec.block_id = "m1";
    rec.user_id = kUser;
    rec.content = "c";
    rec.metadata_json = {
        {"memory_type", "fact"},
        {"status", "active"},
        {"created_at", "not-a-timestamp"},  // unparseable
    };
    MemoryListItem a = MemoryTransparency::ProjectListItem(rec, false);
    EXPECT_FALSE(a.extracted_at.has_value());
}

// ReadTimestampMs: a non-string, non-number timestamp value (e.g. bool) → nullopt
// (line 59 fall-through).
TEST_F(MemoryTransparencyTest, ProjectListItemNonScalarTimestampIsUnset) {
    MemoryBlockRecord rec;
    rec.block_id = "m1";
    rec.user_id = kUser;
    rec.content = "c";
    rec.metadata_json = {
        {"memory_type", "fact"},
        {"status", "active"},
        {"extracted_at", true},   // neither number nor string
        {"created_at", false},
    };
    MemoryListItem a = MemoryTransparency::ProjectListItem(rec, false);
    EXPECT_FALSE(a.extracted_at.has_value());
}

// ReadOptString empty-string branch (line 68): an explain projection where a B-class
// key is present but an empty string must surface as nullopt, not "".
TEST_F(MemoryTransparencyTest, ProjectListItemExplainEmptyStringFieldIsNullopt) {
    MemoryBlockRecord rec = MakeMemory("m1", kUser, "c", "fact", "active");
    rec.metadata_json["extraction_method"] = "";  // present but empty
    rec.metadata_json["source_session_id"] = "sess_x";
    MemoryListItem b = MemoryTransparency::ProjectListItem(rec, /*explain=*/true);
    EXPECT_FALSE(b.extraction_method.has_value());     // empty string -> nullopt
    ASSERT_TRUE(b.source_session_id.has_value());       // non-empty -> present
    EXPECT_EQ(*b.source_session_id, "sess_x");
}

TEST_F(MemoryTransparencyTest, IsEffectiveStatusClassification) {
    EXPECT_TRUE(MemoryTransparency::IsEffectiveStatus(
        MakeMemory("a", kUser, "c", "fact", "active")));
    EXPECT_TRUE(MemoryTransparency::IsEffectiveStatus(
        MakeMemory("b", kUser, "c", "fact", "tentative")));
    EXPECT_FALSE(MemoryTransparency::IsEffectiveStatus(
        MakeMemory("c", kUser, "c", "fact", "invalidated")));
}

TEST_F(MemoryTransparencyTest, TriggeredByToString) {
    EXPECT_STREQ(ToString(TriggeredBy::kUserManual), "user_manual");
    EXPECT_STREQ(ToString(TriggeredBy::kUserEdit), "user_edit");
    EXPECT_STREQ(ToString(TriggeredBy::kAdminRevoke), "admin_revoke");
    EXPECT_STREQ(ToString(TriggeredBy::kLlmAuto), "llm_auto");
}

// ---------- MakeOpLogEntry ns_id true-arm (line 116) ----------
// Existing fixtures had empty ns_id, so the audit's `if (!ns_id.empty())` true arm
// (sets namespace_id) was never hit. Seed a block WITH ns_id and delete/edit it.

TEST_F(MemoryTransparencyTest, DeleteAuditSetsNamespaceIdWhenNsPresent) {
    MemoryBlockRecord rec = MakeMemory("m1", kUser, "c", "fact", "active");
    rec.ns_id = "ns_alpha";  // non-empty -> audit sets namespace_id
    store_->Put(rec);
    ASSERT_TRUE(mt_->Delete("m1", kUser).ok());
    const auto* inv = oplog_->LastWithAction("memory_invalidate");
    ASSERT_NE(inv, nullptr);
    ASSERT_TRUE(inv->namespace_id.has_value());
    EXPECT_EQ(*inv->namespace_id, "ns_alpha");
}

TEST_F(MemoryTransparencyTest, EditAuditSetsNamespaceIdWhenNsPresent) {
    MemoryBlockRecord rec = MakeMemory("m_old", kUser, "c", "fact", "active");
    rec.ns_id = "ns_beta";
    store_->Put(rec);
    MemoryEditRequest req;
    req.memory_id = "m_old";
    req.new_content = "c2";
    auto r = mt_->Edit(req, kUser);
    ASSERT_TRUE(r.ok());
    // memory_edit audit carries the new block's ns_id (forwarded from old_block.ns_id).
    const auto* ed = oplog_->LastWithAction("memory_edit");
    ASSERT_NE(ed, nullptr);
    ASSERT_TRUE(ed->namespace_id.has_value());
    EXPECT_EQ(*ed->namespace_id, "ns_beta");
}

}  // namespace
}  // namespace cortrix::memory::transparency
