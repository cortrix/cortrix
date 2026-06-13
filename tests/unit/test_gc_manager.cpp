#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/config/config.h"

// OPEN-2 three-stage GC manager: state machine + ref_count boundary + second-
// confirm window + restore/purge/dry_run, against a real in-memory catalog.db.
namespace cortrix::catalog::gc {
namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t DaysAgoMs(int days) { return NowMs() - static_cast<int64_t>(days) * 24 * 3600 * 1000; }

// Records every Unlink call so tests can assert which blobs Stage 3 removed.
class RecordingSink : public IBlobGcSink {
public:
    Status Unlink(const std::string& blob_uri) override {
        unlinked.push_back(blob_uri);
        return Status::Ok();
    }
    std::vector<std::string> unlinked;
};

void SeedFile(sqlite3* db, const std::string& fh, const std::string& blob_uri,
              const std::string& status, std::optional<int64_t> deleted_at,
              int64_t size_bytes = 100) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO file_locations (file_hash, unit_id, ns_id, tenant_id, "
        "size_bytes, blob_uri, ref_count, first_seen_at, status, deleted_at, created_at) "
        "VALUES (?, 'u1', 'ns1', 't1', ?, ?, 0, 0, ?, ?, 0)";
    ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, fh.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, size_bytes);
    sqlite3_bind_text(stmt, 3, blob_uri.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, status.c_str(), -1, SQLITE_TRANSIENT);
    if (deleted_at.has_value()) sqlite3_bind_int64(stmt, 5, *deleted_at);
    else sqlite3_bind_null(stmt, 5);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

void SeedRef(sqlite3* db, const std::string& fh, const std::string& doc_id,
             const std::string& status) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO content_refs (file_hash, ns_id, doc_id, target_unit_id, status, created_at) "
        "VALUES (?, 'ns1', ?, '', ?, 0)";
    ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, fh.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, status.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

int CountRows(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK);
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

std::string QueueStatus(sqlite3* db, const std::string& blob_uri) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
                  db, "SELECT status FROM blob_gc_queue WHERE blob_uri=?", -1, &stmt, nullptr),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, blob_uri.c_str(), -1, SQLITE_TRANSIENT);
    std::string s;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (t) s = t;
    }
    sqlite3_finalize(stmt);
    return s;
}

class GcManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        cfg_.soft_delete_retention_days = 30;
        cfg_.blob_gc_retention_days = 90;
    }
    GcManager Make(RecordingSink* sink) { return GcManager(catalog_.db(), sink, cfg_); }

    CatalogDb catalog_;
    GcConfig cfg_;
};

// Stage 2: a file soft-deleted past the retention window with ref_count 0 is
// hard-deleted and its blob enqueued (but NOT unlinked yet — Stage 3 window).
TEST_F(GcManagerTest, Stage2HardDeletesAndEnqueuesPastWindow) {
    SeedFile(catalog_.db(), "fh1", "ab/abhash", "deleted", DaysAgoMs(31));
    RecordingSink sink;
    auto mgr = Make(&sink);

    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().hard_deleted, 1);
    EXPECT_EQ(r.value().blobs_unlinked, 0) << "blob within 90d second-confirm window";

    EXPECT_EQ(CountRows(catalog_.db(), "SELECT COUNT(*) FROM file_locations"), 0);
    EXPECT_EQ(CountRows(catalog_.db(), "SELECT COUNT(*) FROM blob_gc_queue"), 1);
    EXPECT_EQ(QueueStatus(catalog_.db(), "ab/abhash"), "pending");
    EXPECT_TRUE(sink.unlinked.empty());
}

// Stage 2 boundary: a file still within the soft-delete window is untouched.
TEST_F(GcManagerTest, Stage2SkipsWithinWindow) {
    SeedFile(catalog_.db(), "fh1", "ab/abhash", "deleted", DaysAgoMs(10));
    RecordingSink sink;
    auto mgr = Make(&sink);

    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().hard_deleted, 0);
    EXPECT_EQ(CountRows(catalog_.db(), "SELECT COUNT(*) FROM file_locations"), 1);
}

// ref_count boundary: an active content_refs row keeps ref_count>0, so the file
// is NOT hard-deleted even past the window (still referenced).
TEST_F(GcManagerTest, Stage2SkipsWhenStillReferenced) {
    SeedFile(catalog_.db(), "fh1", "ab/abhash", "deleted", DaysAgoMs(31));
    SeedRef(catalog_.db(), "fh1", "docA", "active");  // a live reference remains
    RecordingSink sink;
    auto mgr = Make(&sink);

    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().hard_deleted, 0) << "ref_count>0 blocks hard delete";
    EXPECT_EQ(CountRows(catalog_.db(), "SELECT COUNT(*) FROM file_locations"), 1);
}

// Stage 3: an enqueued blob past eligible_at is unlinked and marked 'unlinked'.
TEST_F(GcManagerTest, Stage3UnlinksPastEligibleWindow) {
    // Enqueue a blob whose eligible_at is already in the past by inserting directly
    // with a queued_at 91 days ago + eligible_at 1 day ago.
    const int64_t queued = DaysAgoMs(91);
    const int64_t eligible = DaysAgoMs(1);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(),
        "INSERT INTO blob_gc_queue (blob_uri, file_hash, queued_at, eligible_at, "
        "last_ref_check, status) VALUES ('cd/cdhash','fh2',?,?,?,'pending')",
        -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, queued);
    sqlite3_bind_int64(stmt, 2, eligible);
    sqlite3_bind_int64(stmt, 3, queued);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);

    RecordingSink sink;
    auto mgr = Make(&sink);
    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().blobs_unlinked, 1);
    ASSERT_EQ(sink.unlinked.size(), 1u);
    EXPECT_EQ(sink.unlinked[0], "cd/cdhash");
    EXPECT_EQ(QueueStatus(catalog_.db(), "cd/cdhash"), "unlinked");
}

// Stage 3 window boundary: an enqueued blob not yet eligible stays pending.
TEST_F(GcManagerTest, Stage3SkipsBeforeEligible) {
    SeedFile(catalog_.db(), "fh1", "ab/abhash", "deleted", DaysAgoMs(31));
    RecordingSink sink;
    auto mgr = Make(&sink);
    // First sweep: Stage 2 enqueues with eligible_at = now + 90d (future).
    ASSERT_TRUE(mgr.RunOnce().ok());
    // Second sweep: blob is still within the 90d window → not unlinked.
    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().blobs_unlinked, 0);
    EXPECT_TRUE(sink.unlinked.empty());
    EXPECT_EQ(QueueStatus(catalog_.db(), "ab/abhash"), "pending");
}

// Purge bypasses both windows: a freshly soft-deleted file is hard-deleted AND its
// blob unlinked in one call.
TEST_F(GcManagerTest, PurgeBypassesWindows) {
    SeedFile(catalog_.db(), "fh1", "ab/abhash", "deleted", DaysAgoMs(1));  // fresh
    RecordingSink sink;
    auto mgr = Make(&sink);

    auto r = mgr.Purge();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().hard_deleted, 1);
    EXPECT_EQ(r.value().blobs_unlinked, 1);
    EXPECT_EQ(CountRows(catalog_.db(), "SELECT COUNT(*) FROM file_locations"), 0);
    ASSERT_EQ(sink.unlinked.size(), 1u);
    EXPECT_EQ(sink.unlinked[0], "ab/abhash");
}

// dry_run scans + reports but mutates nothing.
TEST_F(GcManagerTest, DryRunMutatesNothing) {
    SeedFile(catalog_.db(), "fh1", "ab/abhash", "deleted", DaysAgoMs(31));
    cfg_.dry_run = true;
    RecordingSink sink;
    auto mgr = Make(&sink);

    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().hard_deleted, 1) << "reports what it would delete";
    EXPECT_EQ(CountRows(catalog_.db(), "SELECT COUNT(*) FROM file_locations"), 1)
        << "but does not actually delete";
    EXPECT_TRUE(sink.unlinked.empty());
}

// Restore reverses Stage 1: status/deleted_at cleared, content_refs reactivated.
TEST_F(GcManagerTest, RestoreReactivatesSoftDeleted) {
    SeedFile(catalog_.db(), "fh1", "ab/abhash", "deleted", DaysAgoMs(5));
    SeedRef(catalog_.db(), "fh1", "docA", "deleted");
    RecordingSink sink;
    auto mgr = Make(&sink);

    auto r = mgr.Restore({"docA"});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().succeeded.size(), 1u);
    EXPECT_TRUE(r.value().failed.empty());

    EXPECT_EQ(CountRows(catalog_.db(),
              "SELECT COUNT(*) FROM file_locations WHERE status='active' AND deleted_at IS NULL"), 1);
    EXPECT_EQ(CountRows(catalog_.db(),
              "SELECT COUNT(*) FROM content_refs WHERE status='active'"), 1);
    EXPECT_EQ(CountRows(catalog_.db(),
              "SELECT ref_count FROM file_locations WHERE file_hash='fh1'"), 1)
        << "ref_count recomputed from active content_refs";
}

// Restore of an unknown doc_id is reported as a per-id failure, not a hard error.
TEST_F(GcManagerTest, RestoreUnknownDocReportsFailure) {
    RecordingSink sink;
    auto mgr = Make(&sink);
    auto r = mgr.Restore({"ghost"});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().succeeded.empty());
    ASSERT_EQ(r.value().failed.size(), 1u);
    EXPECT_EQ(r.value().failed[0].first, "ghost");
    EXPECT_EQ(r.value().failed[0].second, "CX_ERR_GC_DOC_NOT_FOUND");
}

// Status reports the Stage 1 backlog + reclaimable bytes + last_gc_at.
TEST_F(GcManagerTest, StatusReportsBacklogAndLastRun) {
    SeedFile(catalog_.db(), "fh1", "ab/abhash", "deleted", DaysAgoMs(5), /*size=*/256);
    RecordingSink sink;
    auto mgr = Make(&sink);

    auto s0 = mgr.GetStatus();
    ASSERT_TRUE(s0.ok());
    EXPECT_EQ(s0.value().soft_deleted_count, 1);
    EXPECT_EQ(s0.value().reclaimable_bytes, 256);
    EXPECT_FALSE(s0.value().last_gc_at_ms.has_value()) << "no sweep yet";

    ASSERT_TRUE(mgr.RunOnce().ok());
    auto s1 = mgr.GetStatus();
    ASSERT_TRUE(s1.ok());
    EXPECT_TRUE(s1.value().last_gc_at_ms.has_value());
}

// max_purge_per_run caps how many files Stage 2 removes in a single sweep.
TEST_F(GcManagerTest, MaxPurgePerRunCapsStage2) {
    SeedFile(catalog_.db(), "fh1", "a/1", "deleted", DaysAgoMs(31));
    SeedFile(catalog_.db(), "fh2", "a/2", "deleted", DaysAgoMs(31));
    SeedFile(catalog_.db(), "fh3", "a/3", "deleted", DaysAgoMs(31));
    cfg_.max_purge_per_run = 2;
    RecordingSink sink;
    auto mgr = Make(&sink);

    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().hard_deleted, 2);
    EXPECT_TRUE(r.value().hit_run_cap);
    EXPECT_EQ(CountRows(catalog_.db(), "SELECT COUNT(*) FROM file_locations"), 1)
        << "one row left for the next sweep";
}

}  // namespace
}  // namespace cortrix::catalog::gc
