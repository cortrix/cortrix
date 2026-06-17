// GcManager supplemental tests — covers branches not reached by test_gc_manager.cpp
// (currently at 74.8% line coverage). Targets:
//   - EnqueueBlob path (standalone call)
//   - Vacuum / Reindex success paths
//   - Stage 2 no-blob-uri file (no enqueue, only hard-delete)
//   - Stage 3 sink-error path (sink fails → row stays pending)
//   - Restore: multi-doc partial success/failure mix
//   - Restore: already-active content_refs (idempotent reactivation)
//   - dry_run with bypass_windows (Purge + dry_run)
//   - GetStatus while running_ (thread-safe snapshot)
//   - hit_run_cap from Stage 3 (cap reached while unlinking blobs)
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/config/config.h"

namespace cortrix::catalog::gc {
namespace {

// ---------------------------------------------------------------------------
// Helpers — shared with test_gc_manager.cpp (duplicated to avoid a common
// header; both translation units are compiled into the same test binary).
// ---------------------------------------------------------------------------

int64_t NowMsExtra() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t DaysAgoMsExtra(int days) {
    return NowMsExtra() - static_cast<int64_t>(days) * 24 * 3600 * 1000;
}

// Sink that always fails Unlink.
class FailingSink : public IBlobGcSink {
public:
    Status Unlink(const std::string& uri) override {
        attempted.push_back(uri);
        return Status::Internal("CX_ERR_GC_SINK_FAILED: forced failure");
    }
    std::vector<std::string> attempted;
};

// Sink that succeeds.
class RecordingSinkExtra : public IBlobGcSink {
public:
    Status Unlink(const std::string& uri) override {
        unlinked.push_back(uri);
        return Status::Ok();
    }
    std::vector<std::string> unlinked;
};

void SeedFileExtra(sqlite3* db, const std::string& fh, const std::string& blob_uri,
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

void SeedRefExtra(sqlite3* db, const std::string& fh, const std::string& doc_id,
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

int CountRowsExtra(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK);
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

std::string QueueStatusExtra(sqlite3* db, const std::string& blob_uri) {
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

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class GcManagerExtraTest : public ::testing::Test {
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
    GcManager Make(IBlobGcSink* sink) { return GcManager(catalog_.db(), sink, cfg_); }

    CatalogDb catalog_;
    GcConfig cfg_;
};

// ---------------------------------------------------------------------------
// EnqueueBlob — standalone call path (not triggered via Stage 2).
// ---------------------------------------------------------------------------

// EnqueueBlob with a valid uri+hash inserts a pending row.
TEST_F(GcManagerExtraTest, EnqueueBlob_InsertsRow) {
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);

    auto s = mgr.EnqueueBlob("blob://host/path", "file_hash_1");
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(QueueStatusExtra(catalog_.db(), "blob://host/path"), "pending");
}

// EnqueueBlob is idempotent: a second call on the same blob_uri is a no-op.
TEST_F(GcManagerExtraTest, EnqueueBlob_Idempotent) {
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);

    ASSERT_TRUE(mgr.EnqueueBlob("blob://host/same", "fh").ok());
    ASSERT_TRUE(mgr.EnqueueBlob("blob://host/same", "fh").ok());  // ON CONFLICT DO NOTHING
    EXPECT_EQ(CountRowsExtra(catalog_.db(),
              "SELECT COUNT(*) FROM blob_gc_queue WHERE blob_uri='blob://host/same'"), 1);
}

// EnqueueBlob sets eligible_at > now (blob_gc_retention_days into the future).
TEST_F(GcManagerExtraTest, EnqueueBlob_EligibleAtInFuture) {
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);

    int64_t before = NowMsExtra();
    ASSERT_TRUE(mgr.EnqueueBlob("blob://future", "fh-f").ok());
    int64_t after = NowMsExtra();

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(),
        "SELECT eligible_at FROM blob_gc_queue WHERE blob_uri='blob://future'",
        -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int64_t eligible_at = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    int64_t min_expected = before + static_cast<int64_t>(cfg_.blob_gc_retention_days) * 24 * 3600 * 1000;
    int64_t max_expected = after  + static_cast<int64_t>(cfg_.blob_gc_retention_days) * 24 * 3600 * 1000;
    EXPECT_GE(eligible_at, min_expected);
    EXPECT_LE(eligible_at, max_expected + 5000);  // 5s tolerance
}

// ---------------------------------------------------------------------------
// Vacuum / Reindex success paths.
// ---------------------------------------------------------------------------

TEST_F(GcManagerExtraTest, Vacuum_Succeeds) {
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);
    auto s = mgr.Vacuum();
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST_F(GcManagerExtraTest, Reindex_Succeeds) {
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);
    auto s = mgr.Reindex();
    EXPECT_TRUE(s.ok()) << s.message();
}

// ---------------------------------------------------------------------------
// Stage 2: a soft-deleted file with NO blob_uri (empty string) — the enqueue
// branch is skipped but the file row is still hard-deleted.
// ---------------------------------------------------------------------------

TEST_F(GcManagerExtraTest, Stage2HardDeletes_NoBlobUri_NoEnqueue) {
    // blob_uri = "" → no enqueue into blob_gc_queue
    SeedFileExtra(catalog_.db(), "fh-noob", "", "deleted", DaysAgoMsExtra(31));
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);

    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().hard_deleted, 1);
    EXPECT_EQ(CountRowsExtra(catalog_.db(), "SELECT COUNT(*) FROM file_locations"), 0);
    EXPECT_EQ(CountRowsExtra(catalog_.db(), "SELECT COUNT(*) FROM blob_gc_queue"), 0);
    EXPECT_TRUE(sink.unlinked.empty());
}

// ---------------------------------------------------------------------------
// Stage 3: sink fails → the blob row stays 'pending', reported as not unlinked.
// ---------------------------------------------------------------------------

TEST_F(GcManagerExtraTest, Stage3SinkFails_BlobStaysPending) {
    // Directly insert a blob_gc_queue row that is already eligible.
    const int64_t queued   = DaysAgoMsExtra(91);
    const int64_t eligible = DaysAgoMsExtra(1);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(),
        "INSERT INTO blob_gc_queue (blob_uri, file_hash, queued_at, eligible_at, "
        "last_ref_check, status) VALUES ('blob://fail', 'fh-fail', ?, ?, ?, 'pending')",
        -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, queued);
    sqlite3_bind_int64(stmt, 2, eligible);
    sqlite3_bind_int64(stmt, 3, queued);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);

    FailingSink sink;
    auto mgr = Make(&sink);
    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok()) << r.status().message();
    // Sink was called but returned error → blobs_unlinked stays 0.
    EXPECT_EQ(r.value().blobs_unlinked, 0);
    ASSERT_EQ(sink.attempted.size(), 1u);
    EXPECT_EQ(sink.attempted[0], "blob://fail");
    // Row must stay 'pending' (will be retried next sweep).
    EXPECT_EQ(QueueStatusExtra(catalog_.db(), "blob://fail"), "pending");
}

// ---------------------------------------------------------------------------
// Stage 3 cap: max_purge_per_run limits Stage 3 blob unlinks too.
// ---------------------------------------------------------------------------

TEST_F(GcManagerExtraTest, Stage3MaxPurgeCapLimitsUnlinks) {
    // Insert 3 already-eligible blobs.
    for (int i = 0; i < 3; ++i) {
        std::string uri = "blob://cap/" + std::to_string(i);
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(),
            "INSERT INTO blob_gc_queue (blob_uri, file_hash, queued_at, eligible_at, "
            "last_ref_check, status) VALUES (?, 'fh', ?, ?, ?, 'pending')",
            -1, &st, nullptr), SQLITE_OK);
        sqlite3_bind_text(st, 1, uri.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, DaysAgoMsExtra(91));
        sqlite3_bind_int64(st, 3, DaysAgoMsExtra(1));
        sqlite3_bind_int64(st, 4, DaysAgoMsExtra(91));
        ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
        sqlite3_finalize(st);
    }

    cfg_.max_purge_per_run = 2;
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);

    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().blobs_unlinked, 2);
    EXPECT_TRUE(r.value().hit_run_cap);
    EXPECT_EQ(sink.unlinked.size(), 2u);
    // One row still pending.
    EXPECT_EQ(CountRowsExtra(catalog_.db(),
              "SELECT COUNT(*) FROM blob_gc_queue WHERE status='pending'"), 1);
}

// ---------------------------------------------------------------------------
// Restore: partial success + failure across multiple doc_ids.
// ---------------------------------------------------------------------------

TEST_F(GcManagerExtraTest, Restore_PartialSuccessAndFailure) {
    // doc-good has a valid content_ref that we can reactivate.
    SeedFileExtra(catalog_.db(), "fh-good", "blob://good", "deleted", DaysAgoMsExtra(5));
    SeedRefExtra(catalog_.db(), "fh-good", "doc-good", "deleted");

    RecordingSinkExtra sink;
    auto mgr = Make(&sink);

    auto r = mgr.Restore({"doc-good", "doc-bad"});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().succeeded.size(), 1u);
    EXPECT_EQ(r.value().succeeded[0], "doc-good");
    ASSERT_EQ(r.value().failed.size(), 1u);
    EXPECT_EQ(r.value().failed[0].first, "doc-bad");
    EXPECT_EQ(r.value().failed[0].second, "CX_ERR_GC_DOC_NOT_FOUND");
}

// Restore of multiple valid doc_ids in one call — all succeed.
TEST_F(GcManagerExtraTest, Restore_MultipleDocsAllSucceed) {
    SeedFileExtra(catalog_.db(), "fh1", "b1", "deleted", DaysAgoMsExtra(2));
    SeedFileExtra(catalog_.db(), "fh2", "b2", "deleted", DaysAgoMsExtra(3));
    SeedRefExtra(catalog_.db(), "fh1", "doc1", "deleted");
    SeedRefExtra(catalog_.db(), "fh2", "doc2", "deleted");

    RecordingSinkExtra sink;
    auto mgr = Make(&sink);

    auto r = mgr.Restore({"doc1", "doc2"});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().succeeded.size(), 2u);
    EXPECT_TRUE(r.value().failed.empty());

    // Both files back to active.
    EXPECT_EQ(CountRowsExtra(catalog_.db(),
              "SELECT COUNT(*) FROM file_locations WHERE status='active'"), 2);
}

// Restore of an empty doc list is a no-op success.
TEST_F(GcManagerExtraTest, Restore_EmptyDocList_NoOp) {
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);
    auto r = mgr.Restore({});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().succeeded.empty());
    EXPECT_TRUE(r.value().failed.empty());
}

// ---------------------------------------------------------------------------
// dry_run + Purge (bypass_windows + dry_run: scans but mutates nothing).
// ---------------------------------------------------------------------------

TEST_F(GcManagerExtraTest, DryRunPurge_MutatesNothing) {
    SeedFileExtra(catalog_.db(), "fh1", "b1", "deleted", DaysAgoMsExtra(1));
    cfg_.dry_run = true;
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);

    auto r = mgr.Purge();
    ASSERT_TRUE(r.ok());
    // Reports what it would do.
    EXPECT_EQ(r.value().hard_deleted, 1);
    // But file row is untouched.
    EXPECT_EQ(CountRowsExtra(catalog_.db(), "SELECT COUNT(*) FROM file_locations"), 1);
    EXPECT_TRUE(sink.unlinked.empty());
}

// ---------------------------------------------------------------------------
// GetStatus: soft_deleted_count and reclaimable_bytes are computed from the
// real file_locations rows, not from cached state.
// ---------------------------------------------------------------------------

TEST_F(GcManagerExtraTest, GetStatus_ZeroInitialState) {
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);
    auto s = mgr.GetStatus();
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value().soft_deleted_count, 0);
    EXPECT_EQ(s.value().reclaimable_bytes, 0);
    EXPECT_FALSE(s.value().running);
    EXPECT_FALSE(s.value().last_gc_at_ms.has_value());
}

// Multiple soft-deleted rows are all counted.
TEST_F(GcManagerExtraTest, GetStatus_MultipleDeletedRows) {
    SeedFileExtra(catalog_.db(), "fh1", "b1", "deleted", DaysAgoMsExtra(5), 200);
    SeedFileExtra(catalog_.db(), "fh2", "b2", "deleted", DaysAgoMsExtra(5), 300);
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);

    auto s = mgr.GetStatus();
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value().soft_deleted_count, 2);
    // reclaimable_bytes: only rows with ref_count <= 0 (both seeded with 0).
    EXPECT_EQ(s.value().reclaimable_bytes, 500);
}

// After a successful RunOnce, last_gc_at_ms is set.
TEST_F(GcManagerExtraTest, GetStatus_AfterRunOnce_LastGcAtSet) {
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);
    ASSERT_TRUE(mgr.RunOnce().ok());
    auto s = mgr.GetStatus();
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(s.value().last_gc_at_ms.has_value());
    EXPECT_GT(*s.value().last_gc_at_ms, 0);
}

// ---------------------------------------------------------------------------
// config() accessor returns the GcConfig passed at construction.
// ---------------------------------------------------------------------------

TEST_F(GcManagerExtraTest, ConfigAccessor_ReturnsCorrectConfig) {
    cfg_.soft_delete_retention_days = 15;
    cfg_.blob_gc_retention_days = 45;
    cfg_.max_purge_per_run = 7;
    RecordingSinkExtra sink;
    auto mgr = Make(&sink);
    EXPECT_EQ(mgr.config().soft_delete_retention_days, 15);
    EXPECT_EQ(mgr.config().blob_gc_retention_days, 45);
    EXPECT_EQ(mgr.config().max_purge_per_run, 7);
}

// ---------------------------------------------------------------------------
// Null sink: Stage 3 treats a null sink as "always succeeds" (Status::Ok()
// returned by the `sink_ ? sink_->Unlink(uri) : Status::Ok()` branch).
// ---------------------------------------------------------------------------

TEST_F(GcManagerExtraTest, Stage3NullSink_TreatedAsSuccess) {
    const int64_t queued   = DaysAgoMsExtra(91);
    const int64_t eligible = DaysAgoMsExtra(1);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(),
        "INSERT INTO blob_gc_queue (blob_uri, file_hash, queued_at, eligible_at, "
        "last_ref_check, status) VALUES ('blob://null-sink', 'fh-ns', ?, ?, ?, 'pending')",
        -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, queued);
    sqlite3_bind_int64(stmt, 2, eligible);
    sqlite3_bind_int64(stmt, 3, queued);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);

    auto mgr = GcManager(catalog_.db(), /*sink=*/nullptr, cfg_);
    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().blobs_unlinked, 1);
    EXPECT_EQ(QueueStatusExtra(catalog_.db(), "blob://null-sink"), "unlinked");
}

}  // namespace
}  // namespace cortrix::catalog::gc
