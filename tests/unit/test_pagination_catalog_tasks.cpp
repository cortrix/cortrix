// Pagination + filter matrices for catalog ns_router ListNamespaces and the
// async TaskManager ListByNamespace (F23 breadth).
//
// DefaultINSRouter (include/cortrix/catalog/default_ns_router.h):
//   explicit DefaultINSRouter(sqlite3* db, resource::INamespacePool* = nullptr)
//   Result<BatchResult<std::string>> ListNamespaces(const ListNamespacesOptions&)
//   ListNamespacesOptions{ optional<string> tenant_id, bool include_deleted=false,
//                          int limit=0, int offset=0 }
//   SQL: ORDER BY ns_id ASC; "LIMIT n" only emitted when limit>0; "OFFSET n" only
//   when offset>0. CONSEQUENCE (verified against SQLite): offset>0 with limit<=0
//   builds a bare "OFFSET" with no preceding "LIMIT" => prepare syntax error =>
//   ListNamespaces returns an error (kInternalError). offset>0 is only valid
//   together with limit>0. limit<=0 => all rows. Seeding needs a tenant row (FK).
//
// TaskManager (include/cortrix/async/task_manager.h):
//   Status Init(db_path=":memory:")
//   Result<TaskInfo> CreateTask(TaskInfo)
//   Result<std::vector<TaskInfo>> ListByNamespace(ns_id, int limit, int offset)
//   SQL: WHERE namespace_id=? ORDER BY created_at DESC LIMIT ? OFFSET ?
//   limit/offset bound raw (no clamp). SQLite: LIMIT<0 => all rows; OFFSET<0 => 0.
//   Standalone store, no FK / no tenant seeding.
//
// Suite/fixture names globally unique: NsListPageMatrix*, TaskListPageMatrix*.
// Deterministic: in-memory SQLite; no network/LLM.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cortrix/async/task_manager.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/default_ns_router.h"

// ===========================================================================
// DefaultINSRouter::ListNamespaces - limit/offset + tenant filter matrix.
// ===========================================================================
namespace cortrix {
namespace catalog {
namespace {

struct NsListPageCase {
    int seed_count;     // namespaces seeded under tenant "t1"
    int limit;
    int offset;
    bool expect_ok;     // false when offset>0 && limit<=0 (bare OFFSET syntax error)
    int expect_rows;    // checked only when expect_ok
};

class NsListPageMatrix : public ::testing::TestWithParam<NsListPageCase> {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        router_ = std::make_unique<DefaultINSRouter>(catalog_.db());
    }
    void Seed(int n) {
        for (int i = 0; i < n; ++i) {
            NSMetadata m;
            // Zero-padded so ns_id ASC order is deterministic.
            char id[16];
            std::snprintf(id, sizeof(id), "ns_%03d", i);
            m.namespace_id = id;
            m.name = id;
            m.tenant_id = "t1";
            ASSERT_TRUE(router_->CreateNamespace(m).ok());
        }
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultINSRouter> router_;
};

TEST_P(NsListPageMatrix, LimitOffset) {
    const auto& p = GetParam();
    Seed(p.seed_count);

    ListNamespacesOptions opts;
    opts.tenant_id = std::string("t1");
    opts.limit = p.limit;
    opts.offset = p.offset;
    auto res = router_->ListNamespaces(opts);

    if (!p.expect_ok) {
        EXPECT_FALSE(res.ok())
            << "limit=" << p.limit << " offset=" << p.offset
            << " (bare OFFSET expected to fail)";
        return;
    }
    ASSERT_TRUE(res.ok()) << res.status().message();
    EXPECT_EQ(static_cast<int>(res.value().results.size()), p.expect_rows)
        << "seed=" << p.seed_count << " limit=" << p.limit
        << " offset=" << p.offset;
}

INSTANTIATE_TEST_SUITE_P(
    ListNamespacesLimitOffset, NsListPageMatrix,
    ::testing::Values(
        // seed=5, limit only (offset 0 => no OFFSET clause).
        NsListPageCase{5, 0, 0, true, 5},     // limit 0 => all rows
        NsListPageCase{5, -1, 0, true, 5},    // negative => no LIMIT => all
        NsListPageCase{5, 1, 0, true, 1},     // limit 1
        NsListPageCase{5, 3, 0, true, 3},     // partial
        NsListPageCase{5, 5, 0, true, 5},     // exactly count
        NsListPageCase{5, 10, 0, true, 5},    // oversize > count
        // valid offset (paired with limit>0).
        NsListPageCase{5, 2, 2, true, 2},     // mid page
        NsListPageCase{5, 2, 4, true, 1},     // tail partial
        NsListPageCase{5, 2, 5, true, 0},     // offset == count => empty
        NsListPageCase{5, 2, 99, true, 0},    // offset beyond count => empty
        NsListPageCase{5, 10, 1, true, 4},    // skip 1
        // offset>0 with limit<=0 => bare OFFSET => prepare error.
        NsListPageCase{5, 0, 2, false, 0},
        NsListPageCase{5, -1, 3, false, 0},
        // empty + single.
        NsListPageCase{0, 10, 0, true, 0},
        NsListPageCase{0, 0, 0, true, 0},
        NsListPageCase{1, 10, 0, true, 1}));

// ---- ns_router: tenant filter match / no-match / NS-wide --------------------

class NsListPageFilter : public NsListPageMatrix {};

TEST_F(NsListPageFilter, TenantFilterIsolatesAndNoMatchEmpty) {
    // second tenant t2 with its own NS.
    ASSERT_EQ(sqlite3_exec(catalog_.db(),
        "INSERT INTO tenants(tenant_id, created_at) VALUES('t2', 0)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    Seed(3);  // 3 under t1
    NSMetadata m;
    m.namespace_id = "ns_t2";
    m.name = "ns_t2";
    m.tenant_id = "t2";
    ASSERT_TRUE(router_->CreateNamespace(m).ok());

    ListNamespacesOptions t1;
    t1.tenant_id = std::string("t1");
    auto r1 = router_->ListNamespaces(t1);
    ASSERT_TRUE(r1.ok()) << r1.status().message();
    EXPECT_EQ(r1.value().results.size(), 3u);

    ListNamespacesOptions t2;
    t2.tenant_id = std::string("t2");
    auto r2 = router_->ListNamespaces(t2);
    ASSERT_TRUE(r2.ok()) << r2.status().message();
    EXPECT_EQ(r2.value().results.size(), 1u);

    ListNamespacesOptions tx;
    tx.tenant_id = std::string("no_such_tenant");
    auto rx = router_->ListNamespaces(tx);
    ASSERT_TRUE(rx.ok()) << rx.status().message();
    EXPECT_TRUE(rx.value().results.empty());

    // No tenant filter => NS-wide (t1's 3 + t2's 1).
    ListNamespacesOptions all;
    auto ra = router_->ListNamespaces(all);
    ASSERT_TRUE(ra.ok()) << ra.status().message();
    EXPECT_EQ(ra.value().results.size(), 4u);
}

TEST_F(NsListPageFilter, OrderingAscAndPartitionsAcrossPages) {
    Seed(6);  // ns_000 .. ns_005
    // Single full list is sorted ascending.
    ListNamespacesOptions full;
    full.tenant_id = std::string("t1");
    full.limit = 100;
    auto rf = router_->ListNamespaces(full);
    ASSERT_TRUE(rf.ok()) << rf.status().message();
    const auto& ids = rf.value().results;
    ASSERT_EQ(ids.size(), 6u);
    for (size_t i = 1; i < ids.size(); ++i) EXPECT_LT(ids[i - 1], ids[i]);

    // Pages of 2 concatenate back to the full ordered list.
    std::vector<std::string> paged;
    for (int off = 0; off < 6; off += 2) {
        ListNamespacesOptions pg;
        pg.tenant_id = std::string("t1");
        pg.limit = 2;
        pg.offset = off;
        auto r = router_->ListNamespaces(pg);
        ASSERT_TRUE(r.ok()) << r.status().message();
        for (const auto& s : r.value().results) paged.push_back(s);
    }
    EXPECT_EQ(paged, ids);
}

}  // namespace
}  // namespace catalog
}  // namespace cortrix

// ===========================================================================
// TaskManager::ListByNamespace - limit/offset (raw bind) + ns filter matrix.
// ===========================================================================
namespace cortrix {
namespace async {
namespace {

struct TaskListPageCase {
    int seed_count;     // tasks seeded under "nsA"
    int limit;
    int offset;
    int expect_rows;
};

class TaskListPageMatrix : public ::testing::TestWithParam<TaskListPageCase> {
protected:
    void SetUp() override {
        ASSERT_TRUE(mgr_.Init(":memory:").ok());
    }
    void Seed(const std::string& ns, int n) {
        for (int i = 0; i < n; ++i) {
            TaskInfo t;
            t.namespace_id = ns;
            t.filename = "f" + std::to_string(i) + ".pdf";
            t.filepath = "/tmp/f" + std::to_string(i) + ".pdf";
            t.content_hash = "h_" + ns + "_" + std::to_string(i);
            t.total_pages = 10;
            auto r = mgr_.CreateTask(t);
            ASSERT_TRUE(r.ok()) << r.status().message();
        }
    }
    TaskManager mgr_;
};

TEST_P(TaskListPageMatrix, LimitOffset) {
    const auto& p = GetParam();
    Seed("nsA", p.seed_count);

    auto res = mgr_.ListByNamespace("nsA", p.limit, p.offset);
    ASSERT_TRUE(res.ok()) << res.status().message();
    EXPECT_EQ(static_cast<int>(res.value().size()), p.expect_rows)
        << "seed=" << p.seed_count << " limit=" << p.limit
        << " offset=" << p.offset;
    for (const auto& t : res.value())
        EXPECT_EQ(t.namespace_id, "nsA");
}

INSTANTIATE_TEST_SUITE_P(
    ListByNamespaceLimitOffset, TaskListPageMatrix,
    ::testing::Values(
        TaskListPageCase{5, 0, 0, 0},      // limit 0 => none
        TaskListPageCase{5, 1, 0, 1},      // limit 1
        TaskListPageCase{5, 5, 0, 5},      // exactly count
        TaskListPageCase{5, 3, 0, 3},      // partial
        TaskListPageCase{5, 10, 0, 5},     // oversize > count
        TaskListPageCase{5, -1, 0, 5},     // negative limit => all rows
        TaskListPageCase{5, 2, 2, 2},      // offset mid
        TaskListPageCase{5, 2, 4, 1},      // tail partial
        TaskListPageCase{5, 2, 5, 0},      // offset == count => empty
        TaskListPageCase{5, 2, 99, 0},     // offset beyond count => empty
        TaskListPageCase{5, 3, -1, 3},     // negative offset => 0
        TaskListPageCase{0, 10, 0, 0},     // empty namespace
        TaskListPageCase{1, 10, 0, 1}));   // single

// ---- task_manager: namespace filter match / no-match ------------------------

class TaskListPageFilter : public TaskListPageMatrix {};

TEST_F(TaskListPageFilter, NamespaceScopedAndNoMatchEmpty) {
    Seed("nsA", 3);
    Seed("nsB", 2);

    auto a = mgr_.ListByNamespace("nsA", 100, 0);
    auto b = mgr_.ListByNamespace("nsB", 100, 0);
    auto none = mgr_.ListByNamespace("ns_absent", 100, 0);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(none.ok());
    EXPECT_EQ(a.value().size(), 3u);
    EXPECT_EQ(b.value().size(), 2u);
    EXPECT_TRUE(none.value().empty());
}

TEST_F(TaskListPageFilter, PagingPartitionsFullSetNoOverlap) {
    Seed("nsP", 6);
    // Page through with limit=2; union of task_ids must be all 6, no overlap.
    std::set<std::string> seen;
    int total = 0;
    for (int off = 0; off < 6; off += 2) {
        auto r = mgr_.ListByNamespace("nsP", 2, off);
        ASSERT_TRUE(r.ok()) << r.status().message();
        for (const auto& t : r.value()) {
            seen.insert(t.task_id);
            ++total;
        }
    }
    EXPECT_EQ(total, 6);
    EXPECT_EQ(seen.size(), 6u);  // no dupes across pages
}

}  // namespace
}  // namespace async
}  // namespace cortrix
