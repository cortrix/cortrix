// Issue #72 -- the F42 dispatch query must be served by a composite (status,
// created_at) index instead of scanning or sorting the queued rows.
//
// Why this is a test and not just a schema line: the dispatch SELECT runs on every
// worker's every dequeue attempt while the TaskManager mutex is held, so an O(queued)
// plan there gates the whole pool (measured on-prem: in-flight tasks pinned at 2-5
// regardless of worker count at ~71k queued rows). A plan regression is invisible in
// behaviour tests -- results stay correct, only throughput collapses -- so the plan
// itself is pinned here, in both shapes the scheduler actually issues.
//
// Deterministic: a temp-file TaskManager (the plan is read back over the same file
// with a second sqlite handle), fixed row counts, no sleeps.

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/common/status.h"

namespace cortrix::async {
namespace {

constexpr const char* kIndexName = "idx_tasks_status_created";

// The column list is irrelevant to the plan; the FROM/WHERE/ORDER BY shape is what
// SelectOldestQueuedTaskExcluding builds, so the test pins that shape verbatim.
std::string DispatchSql(int exclusion_pairs) {
    std::string sql = "SELECT task_id FROM tasks WHERE status='queued'";
    for (int i = 0; i < exclusion_pairs; ++i) {
        sql += " AND (doc_id IS NULL OR NOT (namespace_id = ? AND doc_id = ?))";
    }
    sql += " ORDER BY created_at ASC LIMIT 1";
    return sql;
}

class F42DispatchIndexFx : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (std::filesystem::temp_directory_path() /
                 ("cortrix_f42_dispatch_index_" +
                  std::to_string(::testing::UnitTest::GetInstance()
                                     ->current_test_info()
                                     ->line())) )
                    .string() + ".db";
        std::filesystem::remove(path_);
        std::filesystem::remove(path_ + "-wal");
        std::filesystem::remove(path_ + "-shm");
        ASSERT_TRUE(mgr_.Init(path_).ok());
        ASSERT_EQ(sqlite3_open(path_.c_str(), &probe_), SQLITE_OK);
    }

    void TearDown() override {
        if (probe_) sqlite3_close(probe_);
        std::filesystem::remove(path_);
        std::filesystem::remove(path_ + "-wal");
        std::filesystem::remove(path_ + "-shm");
    }

    // Seeds `n` queued tasks so the planner has a populated table to reason about.
    void SeedQueued(int n) {
        for (int i = 0; i < n; ++i) {
            TaskInfo t;
            t.namespace_id = "ns";
            t.filename = "doc.pdf";
            t.filepath = "/tmp/doc" + std::to_string(i) + ".pdf";
            t.doc_id = "doc" + std::to_string(i);
            t.content_hash = "h" + std::to_string(i);
            ASSERT_TRUE(mgr_.CreateTask(t).ok());
        }
    }

    std::string ExplainOne(const std::string& sql) {
        sqlite3_stmt* stmt = nullptr;
        const std::string explain = "EXPLAIN QUERY PLAN " + sql;
        EXPECT_EQ(sqlite3_prepare_v2(probe_, explain.c_str(), -1, &stmt, nullptr),
                  SQLITE_OK)
            << sqlite3_errmsg(probe_);
        std::string plan;
        while (stmt != nullptr && sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* detail = sqlite3_column_text(stmt, 3);
            if (detail != nullptr) {
                plan += reinterpret_cast<const char*>(detail);
                plan += "\n";
            }
        }
        sqlite3_finalize(stmt);
        return plan;
    }

    bool IndexExists(const char* name) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(probe_,
                               "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?",
                               -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        const bool found = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
        return found;
    }

    std::string path_;
    TaskManager mgr_;
    sqlite3* probe_ = nullptr;
};

// ---- schema ----

TEST_F(F42DispatchIndexFx, CompositeIndexCreatedOnFreshDb) {
    EXPECT_TRUE(IndexExists(kIndexName));
}

// A tasks.db created before the index existed must gain it on the next Init.
// Dropping the index and re-running Init reproduces exactly that upgrade path.
TEST_F(F42DispatchIndexFx, CompositeIndexRestoredOnReinitMigration) {
    ASSERT_EQ(sqlite3_exec(probe_, "DROP INDEX idx_tasks_status_created", nullptr,
                           nullptr, nullptr),
              SQLITE_OK);
    ASSERT_FALSE(IndexExists(kIndexName));

    TaskManager reopened;
    ASSERT_TRUE(reopened.Init(path_).ok());
    EXPECT_TRUE(IndexExists(kIndexName));
}

// ---- query plan (the actual regression guard) ----

TEST_F(F42DispatchIndexFx, DispatchPlanUsesIndexWithoutExclusions) {
    SeedQueued(200);
    const std::string plan = ExplainOne(DispatchSql(0));
    EXPECT_NE(plan.find(kIndexName), std::string::npos) << plan;
    EXPECT_EQ(plan.find("SCAN tasks"), std::string::npos) << plan;
    // An ORDER BY served by the index means no sorting pass over the queue.
    EXPECT_EQ(plan.find("TEMP B-TREE"), std::string::npos) << plan;
}

// The worker pool excludes every in-flight (namespace_id, doc_id) pair, so the live
// statement carries one clause per busy worker. The plan must survive that shape --
// a planner that falls back to a scan once the WHERE grows would reintroduce #72.
TEST_F(F42DispatchIndexFx, DispatchPlanUsesIndexWithWorkerPoolExclusions) {
    SeedQueued(200);
    const std::string plan = ExplainOne(DispatchSql(28));
    EXPECT_NE(plan.find(kIndexName), std::string::npos) << plan;
    EXPECT_EQ(plan.find("SCAN tasks"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("TEMP B-TREE"), std::string::npos) << plan;
}

// ---- behaviour is unchanged (claim + exclusion semantics preserved) ----

TEST_F(F42DispatchIndexFx, DequeueStillReturnsOldestQueuedAndSkipsActiveDocs) {
    SeedQueued(3);  // doc0 is oldest, then doc1, doc2

    TaskScheduler sched(&mgr_, /*config=*/nullptr);

    auto first = sched.Dequeue(/*worker_id=*/1);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value().has_value());
    EXPECT_EQ(first.value()->doc_id, "doc0");
    EXPECT_EQ(first.value()->status, task_status::kProcessing);

    // doc0 is now in flight: the next worker must skip it and claim the next oldest,
    // not re-claim the same document.
    auto second = sched.Dequeue(/*worker_id=*/2);
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(second.value().has_value());
    EXPECT_EQ(second.value()->doc_id, "doc1");

    sched.OnTaskCompleted("ns", "doc1");
    ASSERT_TRUE(mgr_.MarkCompleted(second.value()->task_id, "doc1").ok());

    auto third = sched.Dequeue(/*worker_id=*/2);
    ASSERT_TRUE(third.ok());
    ASSERT_TRUE(third.value().has_value());
    EXPECT_EQ(third.value()->doc_id, "doc2");
}

}  // namespace
}  // namespace cortrix::async
