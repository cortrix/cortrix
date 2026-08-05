// Pagination + filter matrices for the agent_trace Query API and the memory transparency
// MemoryTransparency List API (test suite breadth).
//
// agent_trace (include/cortrix/agent_trace/agent_trace_writer_impl.h):
//   AgentTraceWriterImpl(sqlite3*, shared_ptr<IGlobalConfig>)
//   Result<TraceSession> Query(session_id, const TraceFilter&, ctx=nullptr)
//   TraceFilter{ ...optional dims..., int limit=50, int offset=0 }
//   Validation (NOT clamp): limit must be [1,200] else CX_ERR_F13_INVALID_FILTER;
//       offset < 0 => invalid; offset > 0 && offset >= total_count => invalid
//       (offset==0 always OK; rows ORDER BY created_at ASC, id ASC).
//
// transparency (include/cortrix/memory/memory_transparency.h):
//   MemoryTransparency(lister, block_store, op_logger)
//   Result<MemoryListResponse> List(filter, session_user_id, explain=false, ctx)
//   MemoryListFilter{ user_id, include_invalidated=false, opt memory_type,
//                     page=0, page_size=50 }, kMaxPageSize=200.
//   Clamp: page<0 => 0 ; page_size<=0 => 50 ; page_size>200 => 200 + warning.
//   ListByUser returns only blocks owned by user with a memory_type; List then
//   drops invalidated unless include_invalidated; total = post-filter size.
//
// Suite/fixture names globally unique: AgentTracePageMatrix*, MemTranspPageMatrix*.
// Deterministic: in-memory SQLite + local fakes; no network/LLM.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/agent_trace/agent_trace_writer_impl.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/memory/mem03_error.h"
#include "cortrix/memory/memory_transparency.h"

// ===========================================================================
// agent_trace Query - limit/offset validation matrix.
// ===========================================================================
namespace cortrix {
namespace agent_trace {
namespace {

class AgentTracePageGlobalConfig : public cortrix::IGlobalConfig {
public:
    Result<std::string> GetString(const std::string&) const override {
        return Status::InvalidArgument("unused");
    }
    Result<bool> GetBool(const std::string&) const override {
        return Status::InvalidArgument("unused");
    }
    Result<int> GetInt(const std::string&) const override {
        return Status::InvalidArgument("unused");
    }
    Result<float> GetFloat(const std::string&) const override {
        return Status::InvalidArgument("unused");
    }
    void OnChange(std::function<void(const std::string&)>) override {}
};

struct AgentTracePageCase {
    int seed_count;       // traces to seed for the session
    int limit;
    int offset;
    bool expect_ok;       // true => Query returns Ok (paginates); false => invalid filter
    int expect_rows;      // only checked when expect_ok
    bool expect_has_next; // only checked when expect_ok
};

class AgentTracePageMatrix
    : public ::testing::TestWithParam<AgentTracePageCase> {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        config_ = std::make_shared<AgentTracePageGlobalConfig>();
        writer_ = std::make_unique<AgentTraceWriterImpl>(db_, config_);
    }
    void TearDown() override {
        writer_.reset();
        if (db_) sqlite3_close(db_);
    }
    void Seed(const std::string& session, int n) {
        for (int i = 0; i < n; ++i) {
            AgentTraceEntry e;
            e.session_id = session;
            e.method = "cortrix_query";
            e.source = "http";
            e.status = "success";
            e.duration_ms = 10;
            e.created_at = 1000 + i;  // deterministic ascending order
            writer_->Write(e);
        }
    }
    AgentTraceSchemaProvider provider_;
    std::shared_ptr<AgentTracePageGlobalConfig> config_;
    std::unique_ptr<AgentTraceWriterImpl> writer_;
    sqlite3* db_ = nullptr;
};

TEST_P(AgentTracePageMatrix, LimitOffset) {
    const auto& p = GetParam();
    const std::string session = "sess_atp";
    Seed(session, p.seed_count);

    TraceFilter filter;
    filter.limit = p.limit;
    filter.offset = p.offset;
    auto res = writer_->Query(session, filter);

    if (!p.expect_ok) {
        EXPECT_FALSE(res.ok())
            << "limit=" << p.limit << " offset=" << p.offset;
        return;
    }
    ASSERT_TRUE(res.ok()) << res.status().message();
    const TraceSession& ts = res.value();
    EXPECT_EQ(ts.trace_count, p.expect_rows)
        << "seed=" << p.seed_count << " limit=" << p.limit
        << " offset=" << p.offset;
    EXPECT_EQ(static_cast<int>(ts.traces.size()), p.expect_rows);
    EXPECT_EQ(ts.total_count, p.seed_count);
    EXPECT_EQ(ts.has_next, p.expect_has_next);
}

INSTANTIATE_TEST_SUITE_P(
    QueryLimitOffset, AgentTracePageMatrix,
    ::testing::Values(
        // --- invalid limit (rejected, not clamped) ---
        AgentTracePageCase{5, 0, 0, false, 0, false},    // limit 0 < 1
        AgentTracePageCase{5, -3, 0, false, 0, false},   // negative limit
        AgentTracePageCase{5, 201, 0, false, 0, false},  // > 200 cap
        // --- invalid offset ---
        AgentTracePageCase{5, 10, -1, false, 0, false},  // negative offset
        AgentTracePageCase{5, 10, 5, false, 0, false},   // offset == count (count>0)
        AgentTracePageCase{5, 10, 99, false, 0, false},  // offset beyond count
        // --- valid pages ---
        AgentTracePageCase{5, 1, 0, true, 1, true},      // first of 5
        AgentTracePageCase{5, 5, 0, true, 5, false},     // exactly count
        AgentTracePageCase{5, 200, 0, true, 5, false},   // oversize within cap
        AgentTracePageCase{5, 2, 0, true, 2, true},      // page 1
        AgentTracePageCase{5, 2, 2, true, 2, true},      // page 2 (rows 2..3)
        AgentTracePageCase{5, 2, 4, true, 1, false},     // last partial page
        AgentTracePageCase{5, 3, 3, true, 2, false},     // tail
        // --- empty session: offset 0 allowed, returns nothing ---
        AgentTracePageCase{0, 10, 0, true, 0, false},
        AgentTracePageCase{0, 1, 0, true, 0, false},
        // --- single row ---
        AgentTracePageCase{1, 50, 0, true, 1, false}));

// ---- agent_trace: dimension filter match / no-match ------------------------

class AgentTracePageFilter : public AgentTracePageMatrix {};

TEST_F(AgentTracePageFilter, StatusFilterMatchAndNoMatch) {
    const std::string session = "sess_filt";
    // 3 success + 2 failed.
    for (int i = 0; i < 3; ++i) {
        AgentTraceEntry e;
        e.session_id = session; e.method = "m"; e.source = "http";
        e.status = "success"; e.created_at = 100 + i;
        writer_->Write(e);
    }
    for (int i = 0; i < 2; ++i) {
        AgentTraceEntry e;
        e.session_id = session; e.method = "m"; e.source = "http";
        e.status = "failed"; e.created_at = 200 + i;
        writer_->Write(e);
    }
    TraceFilter f;
    f.status = std::string("failed");
    auto hit = writer_->Query(session, f);
    ASSERT_TRUE(hit.ok()) << hit.status().message();
    EXPECT_EQ(hit.value().total_count, 2);
    EXPECT_EQ(hit.value().trace_count, 2);

    TraceFilter g;
    g.status = std::string("cancelled");  // no such row
    auto miss = writer_->Query(session, g);
    ASSERT_TRUE(miss.ok()) << miss.status().message();
    EXPECT_EQ(miss.value().total_count, 0);
    EXPECT_TRUE(miss.value().traces.empty());
}

TEST_F(AgentTracePageFilter, AgentIdFilterIsolates) {
    const std::string session = "sess_ag";
    for (int i = 0; i < 2; ++i) {
        AgentTraceEntry e;
        e.session_id = session; e.method = "m"; e.source = "http";
        e.agent_id = std::string("agent-a"); e.created_at = 10 + i;
        writer_->Write(e);
    }
    AgentTraceEntry b;
    b.session_id = session; b.method = "m"; b.source = "http";
    b.agent_id = std::string("agent-b"); b.created_at = 50;
    writer_->Write(b);

    TraceFilter f;
    f.agent_id = std::string("agent-a");
    auto res = writer_->Query(session, f);
    ASSERT_TRUE(res.ok()) << res.status().message();
    EXPECT_EQ(res.value().total_count, 2);
    for (const auto& t : res.value().traces)
        ASSERT_TRUE(t.agent_id.has_value());
}

TEST_F(AgentTracePageFilter, OrderingAscByCreatedAtAcrossPages) {
    const std::string session = "sess_ord";
    Seed(session, 6);  // created_at 1000..1005

    // Page through with limit=2; concatenated created_at must be ascending & full.
    std::vector<int64_t> paged;
    for (int off = 0; off < 6; off += 2) {
        TraceFilter f;
        f.limit = 2;
        f.offset = off;
        auto r = writer_->Query(session, f);
        ASSERT_TRUE(r.ok()) << r.status().message();
        for (const auto& t : r.value().traces) paged.push_back(t.created_at);
    }
    ASSERT_EQ(paged.size(), 6u);
    for (size_t i = 1; i < paged.size(); ++i)
        EXPECT_LT(paged[i - 1], paged[i]);
    EXPECT_EQ(paged.front(), 1000);
    EXPECT_EQ(paged.back(), 1005);
}

}  // namespace
}  // namespace agent_trace
}  // namespace cortrix

// ===========================================================================
// MemoryTransparency List - page/page_size clamp + filter matrix.
// ===========================================================================
namespace cortrix {
namespace memory {
namespace transparency {
namespace {

// Local fakes (unique names; mirror the seam contracts in memory_transparency.h
// / memory_extractor.h). A single shared map backs the lister + store.
class MemTranspPageStore {
public:
    void Put(const MemoryBlockRecord& rec) { blocks_[rec.block_id] = rec; }
    std::map<std::string, MemoryBlockRecord> blocks_;
};

class MemTranspPageLister : public IMemoryBlockLister {
public:
    explicit MemTranspPageLister(std::shared_ptr<MemTranspPageStore> s)
        : store_(std::move(s)) {}
    Result<std::vector<MemoryBlockRecord>> ListByUser(
        const std::string& user_id) override {
        std::vector<MemoryBlockRecord> out;
        for (const auto& kv : store_->blocks_) {
            const MemoryBlockRecord& rec = kv.second;
            const bool is_memory = rec.metadata_json.is_object() &&
                                   rec.metadata_json.contains("memory_type");
            if (rec.user_id == user_id && is_memory) out.push_back(rec);
        }
        return out;
    }

private:
    std::shared_ptr<MemTranspPageStore> store_;
};

class MemTranspPageBlockStore : public IMemoryBlockStore {
public:
    explicit MemTranspPageBlockStore(std::shared_ptr<MemTranspPageStore> s)
        : store_(std::move(s)) {}
    Result<std::string> InsertMemoryBlock(const MemoryBlockRecord& b) override {
        store_->blocks_[b.block_id] = b;
        return b.block_id;
    }
    Status UpdateMemoryBlock(const MemoryBlockRecord& b) override {
        if (!store_->blocks_.count(b.block_id))
            return Status::NotFound("no such block");
        store_->blocks_[b.block_id] = b;
        return Status::Ok();
    }
    Result<MemoryBlockRecord> GetMemoryBlock(const std::string& id) override {
        auto it = store_->blocks_.find(id);
        if (it == store_->blocks_.end()) return Status::NotFound("no such block");
        return it->second;
    }

private:
    std::shared_ptr<MemTranspPageStore> store_;
};

class MemTranspPageOpLogger : public observability::IOperationLogger {
public:
    void Log(const observability::OperationLogEntry&,
             const observability::TraceContext* = nullptr) override {}
    void BatchLog(const std::vector<observability::OperationLogEntry>&,
                  const observability::TraceContext* = nullptr) override {}
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext* = nullptr) override {
        return observability::OperationLogQueryResult{};
    }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }
};

MemoryBlockRecord MakeMem(const std::string& id, const std::string& user,
                          const std::string& type, const std::string& status) {
    MemoryBlockRecord rec;
    rec.block_id = id;
    rec.user_id = user;
    rec.content = "content " + id;
    rec.metadata_json = {
        {"memory_type", type},
        {"status", status},
        {"user_id", user},
        {"extraction_method", "llm"},
        {"extracted_at", static_cast<int64_t>(500)},
        {"last_modified_at", static_cast<int64_t>(1000)},
    };
    return rec;
}

struct MemTranspPageCase {
    int seed_active;     // active memories owned by kUser
    int page;
    int page_size;
    int expect_rows;     // rows on this page
    int expect_page;     // echoed (clamped) page
    int expect_page_size;// echoed (clamped) page_size
};

constexpr const char* kMemTranspUser = "user_p";

class MemTranspPageMatrix
    : public ::testing::TestWithParam<MemTranspPageCase> {
protected:
    void SetUp() override {
        store_ = std::make_shared<MemTranspPageStore>();
        lister_ = std::make_shared<MemTranspPageLister>(store_);
        block_store_ = std::make_shared<MemTranspPageBlockStore>(store_);
        oplog_ = std::make_shared<MemTranspPageOpLogger>();
        mt_ = std::make_unique<MemoryTransparency>(lister_, block_store_, oplog_);
    }
    void SeedActive(int n) {
        for (int i = 0; i < n; ++i)
            store_->Put(MakeMem("blk_" + std::to_string(i), kMemTranspUser,
                                "fact", "active"));
    }
    std::shared_ptr<MemTranspPageStore> store_;
    std::shared_ptr<MemTranspPageLister> lister_;
    std::shared_ptr<MemTranspPageBlockStore> block_store_;
    std::shared_ptr<MemTranspPageOpLogger> oplog_;
    std::unique_ptr<MemoryTransparency> mt_;
};

TEST_P(MemTranspPageMatrix, PageSizeAndClamp) {
    const auto& p = GetParam();
    SeedActive(p.seed_active);

    MemoryListFilter f;
    f.user_id = kMemTranspUser;
    f.page = p.page;
    f.page_size = p.page_size;
    auto res = mt_->List(f, kMemTranspUser);
    ASSERT_TRUE(res.ok()) << res.status().message();
    const MemoryListResponse& r = res.value();
    EXPECT_EQ(static_cast<int>(r.memories.size()), p.expect_rows)
        << "seed=" << p.seed_active << " page=" << p.page
        << " page_size=" << p.page_size;
    EXPECT_EQ(r.page, p.expect_page);
    EXPECT_EQ(r.page_size, p.expect_page_size);
    EXPECT_EQ(r.total, p.seed_active);  // total is post-filter, all active
}

INSTANTIATE_TEST_SUITE_P(
    ListPageClamp, MemTranspPageMatrix,
    ::testing::Values(
        // seed=5, default page_size if <=0 => 50.
        MemTranspPageCase{5, 0, 50, 5, 0, 50},    // single page holds all
        MemTranspPageCase{5, 0, 2, 2, 0, 2},      // page 0 of size 2
        MemTranspPageCase{5, 1, 2, 2, 1, 2},      // page 1 (rows 2..3)
        MemTranspPageCase{5, 2, 2, 1, 2, 2},      // page 2 partial
        MemTranspPageCase{5, 3, 2, 0, 3, 2},      // page beyond data => empty
        MemTranspPageCase{5, 0, 5, 5, 0, 5},      // exactly count
        MemTranspPageCase{5, 0, 100, 5, 0, 100},  // oversize within cap
        // clamp: page_size<=0 => 50.
        MemTranspPageCase{5, 0, 0, 5, 0, 50},     // page_size 0 => 50
        MemTranspPageCase{5, 0, -4, 5, 0, 50},    // negative => 50
        // clamp: page_size>200 => 200.
        MemTranspPageCase{5, 0, 500, 5, 0, 200},  // oversize => clamp 200
        // clamp: page<0 => 0.
        MemTranspPageCase{5, -2, 2, 2, 0, 2},     // negative page => 0
        // empty + single.
        MemTranspPageCase{0, 0, 50, 0, 0, 50},
        MemTranspPageCase{1, 0, 50, 1, 0, 50}));

class MemTranspPageFilter : public MemTranspPageMatrix {};

TEST_F(MemTranspPageFilter, PageSizeClampEmitsWarning) {
    SeedActive(2);
    MemoryListFilter f;
    f.user_id = kMemTranspUser;
    f.page_size = 9999;
    auto res = mt_->List(f, kMemTranspUser);
    ASSERT_TRUE(res.ok()) << res.status().message();
    EXPECT_EQ(res.value().page_size, kMaxPageSize);
    EXPECT_FALSE(res.value().warnings.empty());
}

TEST_F(MemTranspPageFilter, IncludeInvalidatedToggle) {
    // 2 active + 3 invalidated for kUser.
    store_->Put(MakeMem("a0", kMemTranspUser, "fact", "active"));
    store_->Put(MakeMem("a1", kMemTranspUser, "fact", "active"));
    for (int i = 0; i < 3; ++i)
        store_->Put(MakeMem("inv" + std::to_string(i), kMemTranspUser,
                            "fact", "invalidated"));

    MemoryListFilter active_only;
    active_only.user_id = kMemTranspUser;
    auto r1 = mt_->List(active_only, kMemTranspUser);
    ASSERT_TRUE(r1.ok()) << r1.status().message();
    EXPECT_EQ(r1.value().total, 2);  // invalidated excluded by default

    MemoryListFilter with_inv;
    with_inv.user_id = kMemTranspUser;
    with_inv.include_invalidated = true;
    auto r2 = mt_->List(with_inv, kMemTranspUser);
    ASSERT_TRUE(r2.ok()) << r2.status().message();
    EXPECT_EQ(r2.value().total, 5);  // all 5
}

TEST_F(MemTranspPageFilter, MemoryTypeFilterMatchAndNoMatch) {
    store_->Put(MakeMem("f0", kMemTranspUser, "fact", "active"));
    store_->Put(MakeMem("f1", kMemTranspUser, "fact", "active"));
    store_->Put(MakeMem("p0", kMemTranspUser, "preference", "active"));

    MemoryListFilter facts;
    facts.user_id = kMemTranspUser;
    facts.memory_type = std::string("fact");
    auto r = mt_->List(facts, kMemTranspUser);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().total, 2);

    MemoryListFilter events;
    events.user_id = kMemTranspUser;
    events.memory_type = std::string("event");  // none seeded
    auto r2 = mt_->List(events, kMemTranspUser);
    ASSERT_TRUE(r2.ok()) << r2.status().message();
    EXPECT_EQ(r2.value().total, 0);
    EXPECT_TRUE(r2.value().memories.empty());
}

TEST_F(MemTranspPageFilter, UserIsolationOtherUserEmpty) {
    store_->Put(MakeMem("o0", "other_user", "fact", "active"));
    store_->Put(MakeMem("o1", "other_user", "fact", "active"));

    MemoryListFilter f;
    f.user_id = kMemTranspUser;  // no rows owned by kUser
    auto r = mt_->List(f, kMemTranspUser);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().total, 0);
    EXPECT_TRUE(r.value().memories.empty());
}

TEST_F(MemTranspPageFilter, UserMismatchRejected) {
    SeedActive(3);
    MemoryListFilter f;
    f.user_id = "someone_else";  // != session user => L1 rejection
    auto r = mt_->List(f, kMemTranspUser);
    EXPECT_FALSE(r.ok());
}

TEST_F(MemTranspPageFilter, PagingPartitionsFullSet) {
    SeedActive(6);
    // Page through with page_size=2 (3 pages); union must be all 6, no dupes.
    std::vector<std::string> seen;
    for (int page = 0; page < 3; ++page) {
        MemoryListFilter f;
        f.user_id = kMemTranspUser;
        f.page = page;
        f.page_size = 2;
        auto r = mt_->List(f, kMemTranspUser);
        ASSERT_TRUE(r.ok()) << r.status().message();
        for (const auto& m : r.value().memories) seen.push_back(m.memory_id);
    }
    EXPECT_EQ(seen.size(), 6u);
    std::set<std::string> uniq(seen.begin(), seen.end());
    EXPECT_EQ(uniq.size(), 6u);  // no overlap across pages
}

}  // namespace
}  // namespace transparency
}  // namespace memory
}  // namespace cortrix
