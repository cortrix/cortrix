// Store contracts for one row per document version (#157): the source lookup
// returns the newest live row, a status write never brings back a deleted row,
// older versions are retired by the newer one, and a soft delete records the
// status the row had so a restore can tell a finished version from one that never
// finished. Publication (the ready write and the retirement), the withdraw of a
// version whose publication failed, and the restore that refuses beside a live
// version each change the rows together or not at all. Also covers a store created
// before documents gained the status column.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <optional>
#include <string>

#include "cortrix/store/cortrix_store_sqlite.h"

namespace cortrix {
namespace {

class DocVersionsStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<CortrixStoreSqlite>(":memory:");
        ASSERT_EQ(store_->Open(), 0);
    }

    std::string Create(const std::string& type, const std::string& path, DocStatus status,
                       const std::string& created_at) {
        CortrixDoc d;
        d.source_type = type;
        d.source_path = path;
        d.status = status;
        EXPECT_EQ(store_->doc_create(d), 0);
        // doc_create stamps created_at with the clock; pin it so the order is exact.
        Exec("UPDATE documents SET created_at = '" + created_at + "' WHERE doc_id = '" +
             d.doc_id + "'");
        return d.doc_id;
    }

    void Exec(const std::string& sql) {
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(store_->db_handle(), sql.c_str(), nullptr, nullptr, &err),
                  SQLITE_OK)
            << (err ? err : "") << " :: " << sql;
        sqlite3_free(err);
    }

    std::string Column(const std::string& doc_id, const std::string& column) {
        sqlite3_stmt* st = nullptr;
        const std::string sql = "SELECT " + column + " FROM documents WHERE doc_id = ?";
        EXPECT_EQ(sqlite3_prepare_v2(store_->db_handle(), sql.c_str(), -1, &st, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(st, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
        std::string v = "<no row>";
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(st, 0);
            v = t ? reinterpret_cast<const char*>(t) : "<null>";
        }
        sqlite3_finalize(st);
        return v;
    }

    DocStatus StatusOf(const std::string& doc_id) {
        CortrixDoc d;
        EXPECT_EQ(store_->doc_get(doc_id, d), 0);
        return d.status;
    }

    // Abort the statement that sets `doc_id` to `status`, as a failed write would.
    void AbortWhen(const std::string& name, const std::string& doc_id,
                   const std::string& status) {
        Exec("CREATE TRIGGER " + name + " BEFORE UPDATE OF status ON documents WHEN OLD.doc_id = '" +
             doc_id + "' AND NEW.status = '" + status +
             "' BEGIN SELECT RAISE(ABORT, 'forced'); END");
    }

    // Break a deferred foreign key once `doc_id` takes `status`: the statement succeeds,
    // and the commit that would end it fails with the transaction still open.
    void FailCommitWhen(const std::string& doc_id, const std::string& status) {
        Exec("CREATE TABLE guard_parent (id INTEGER PRIMARY KEY)");
        Exec("CREATE TABLE guard_child (id INTEGER REFERENCES guard_parent(id) "
             "DEFERRABLE INITIALLY DEFERRED)");
        Exec("CREATE TRIGGER fail_commit AFTER UPDATE OF status ON documents WHEN NEW.doc_id = '" +
             doc_id + "' AND NEW.status = '" + status +
             "' BEGIN INSERT INTO guard_child VALUES (42); END");
    }

    bool InTransaction() { return sqlite3_get_autocommit(store_->db_handle()) == 0; }

    std::unique_ptr<CortrixStoreSqlite> store_;
};

constexpr const char* kDay1 = "2026-10-01T00:00:00.000Z";
constexpr const char* kDay2 = "2026-10-02T00:00:00.000Z";
constexpr const char* kDay3 = "2026-10-03T00:00:00.000Z";

TEST_F(DocVersionsStoreTest, SourceLookupReturnsTheNewestLiveRow) {
    Create("http_upload", "a.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    const std::string newest =
        Create("http_upload", "a.md", DocStatus::kPending, "2026-10-03T00:00:00.000Z");
    Create("http_upload", "a.md", DocStatus::kError, "2026-10-02T00:00:00.000Z");
    CortrixDoc got;
    ASSERT_EQ(store_->doc_find_by_source("http_upload", "a.md", got), 0);
    EXPECT_EQ(got.doc_id, newest);
}

TEST_F(DocVersionsStoreTest, SourceLookupSkipsADeletedNewestRow) {
    const std::string older =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    const std::string newer =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-02T00:00:00.000Z");
    ASSERT_EQ(store_->doc_soft_delete(newer, 1000), 0);
    CortrixDoc got;
    ASSERT_EQ(store_->doc_find_by_source("http_upload", "a.md", got), 0);
    EXPECT_EQ(got.doc_id, older);
    ASSERT_EQ(store_->doc_soft_delete(older, 1000), 0);
    EXPECT_EQ(store_->doc_find_by_source("http_upload", "a.md", got), -2);
}

TEST_F(DocVersionsStoreTest, SameMillisecondRowsOrderByDocId) {
    const std::string a =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    const std::string b =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    CortrixDoc got;
    ASSERT_EQ(store_->doc_find_by_source("http_upload", "a.md", got), 0);
    EXPECT_EQ(got.doc_id, std::max(a, b));
}

TEST_F(DocVersionsStoreTest, StatusWriteLeavesADeletedRowDeleted) {
    const std::string id =
        Create("http_upload", "a.md", DocStatus::kProcessing, "2026-10-01T00:00:00.000Z");
    ASSERT_EQ(store_->doc_soft_delete(id, 1000), 0);
    const std::string message_before = Column(id, "error_message");
    EXPECT_EQ(store_->doc_update_status(id, DocStatus::kReady), -2);
    EXPECT_EQ(store_->doc_update_status(id, DocStatus::kError, "late"), -2);
    EXPECT_EQ(StatusOf(id), DocStatus::kDeleted);
    EXPECT_EQ(Column(id, "error_message"), message_before);
    EXPECT_EQ(store_->doc_update_status("no-such-doc", DocStatus::kReady), -2);
}

TEST_F(DocVersionsStoreTest, RetireRetiresOnlyOlderLiveRowsOfTheSameSource) {
    const std::string v1 =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    const std::string v1_error =
        Create("http_upload", "a.md", DocStatus::kError, "2026-10-01T12:00:00.000Z");
    const std::string v2 =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-02T00:00:00.000Z");
    const std::string v3 =
        Create("http_upload", "a.md", DocStatus::kPending, "2026-10-03T00:00:00.000Z");
    const std::string other_path =
        Create("http_upload", "b.md", DocStatus::kReady, "2026-09-01T00:00:00.000Z");
    const std::string other_type =
        Create("watch_dir", "a.md", DocStatus::kReady, "2026-09-01T00:00:00.000Z");
    const std::string already =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-09-30T00:00:00.000Z");
    ASSERT_EQ(store_->doc_soft_delete(already, 500), 0);

    int64_t retired = -1;
    ASSERT_EQ(store_->doc_retire_older_versions(v2, 7000, &retired), 0);
    EXPECT_EQ(retired, 2);
    EXPECT_EQ(StatusOf(v1), DocStatus::kDeleted);
    EXPECT_EQ(StatusOf(v1_error), DocStatus::kDeleted);
    EXPECT_EQ(Column(v1, "deleted_at"), "7000");
    EXPECT_EQ(Column(v1, "deleted_from_status"), "ready");
    EXPECT_EQ(Column(v1_error, "deleted_from_status"), "error");
    EXPECT_EQ(StatusOf(v2), DocStatus::kReady);
    EXPECT_EQ(StatusOf(v3), DocStatus::kPending);
    EXPECT_EQ(StatusOf(other_path), DocStatus::kReady);
    EXPECT_EQ(StatusOf(other_type), DocStatus::kReady);
    EXPECT_EQ(Column(already, "deleted_at"), "500");  // untouched, not counted

    ASSERT_EQ(store_->doc_retire_older_versions(v2, 8000, &retired), 0);
    EXPECT_EQ(retired, 0);
}

TEST_F(DocVersionsStoreTest, SoftDeleteRecordsTheStatusAndARepeatKeepsTheFirst) {
    const std::string ready =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    const std::string pending =
        Create("http_upload", "b.md", DocStatus::kPending, "2026-10-01T00:00:00.000Z");
    ASSERT_EQ(store_->doc_soft_delete(ready, 1000), 0);
    ASSERT_EQ(store_->doc_soft_delete(pending, 1000), 0);
    EXPECT_EQ(Column(ready, "deleted_from_status"), "ready");
    EXPECT_EQ(Column(pending, "deleted_from_status"), "pending");

    ASSERT_EQ(store_->doc_soft_delete(ready, 2000), 0);  // a repeat still matches the row
    EXPECT_EQ(Column(ready, "deleted_from_status"), "ready");
    EXPECT_EQ(Column(ready, "deleted_at"), "2000");
}

TEST_F(DocVersionsStoreTest, RestoreClearsTheRecordedStatus) {
    const std::string id =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    ASSERT_EQ(store_->doc_soft_delete(id, 1000), 0);
    ASSERT_EQ(store_->doc_restore(id), 0);
    EXPECT_EQ(StatusOf(id), DocStatus::kReady);
    EXPECT_EQ(Column(id, "deleted_from_status"), "<null>");
}

TEST_F(DocVersionsStoreTest, RecordedStatusReadReportsEachCase) {
    const std::string deleted =
        Create("http_upload", "a.md", DocStatus::kProcessing, "2026-10-01T00:00:00.000Z");
    ASSERT_EQ(store_->doc_soft_delete(deleted, 1000), 0);
    std::optional<std::string> s;
    ASSERT_EQ(store_->doc_get_deleted_from_status(deleted, &s), 0);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "processing");

    const std::string live =
        Create("http_upload", "b.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    s = std::string("stale value");
    ASSERT_EQ(store_->doc_get_deleted_from_status(live, &s), 0);
    EXPECT_FALSE(s.has_value());

    // Deleted through a plain status write: no status is recorded, so it is unknown.
    const std::string unknown =
        Create("http_upload", "c.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    ASSERT_EQ(store_->doc_update_status(unknown, DocStatus::kDeleted), 0);
    ASSERT_EQ(store_->doc_get_deleted_from_status(unknown, &s), 0);
    EXPECT_FALSE(s.has_value());

    EXPECT_EQ(store_->doc_get_deleted_from_status("no-such-doc", &s), -2);
}

// A store created before documents gained deleted_from_status: every statement
// that would name the column falls back to today's form, and the read answers
// "unknown" instead of failing.
TEST_F(DocVersionsStoreTest, StoreWithoutTheColumnKeepsWorking) {
    const std::string v1 =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    const std::string v2 =
        Create("http_upload", "a.md", DocStatus::kReady, "2026-10-02T00:00:00.000Z");
    // Before this instance's first marker statement, so its probe sees the old shape.
    Exec("ALTER TABLE documents DROP COLUMN deleted_from_status");

    int64_t retired = -1;
    ASSERT_EQ(store_->doc_retire_older_versions(v2, 7000, &retired), 0);
    EXPECT_EQ(retired, 1);
    EXPECT_EQ(StatusOf(v1), DocStatus::kDeleted);
    std::optional<std::string> s = std::string("stale value");
    ASSERT_EQ(store_->doc_get_deleted_from_status(v1, &s), 0);
    EXPECT_FALSE(s.has_value());
    ASSERT_EQ(store_->doc_restore(v1), 0);
    EXPECT_EQ(StatusOf(v1), DocStatus::kReady);
    ASSERT_EQ(store_->doc_soft_delete(v2, 9000), 0);
    EXPECT_EQ(StatusOf(v2), DocStatus::kDeleted);

    // The restore that refuses beside a live version, and the withdraw.
    EXPECT_EQ(store_->doc_restore_unless_superseded(v2), -2);  // v1 is live
    ASSERT_EQ(store_->doc_soft_delete(v1, 9100), 0);
    EXPECT_EQ(store_->doc_restore_unless_superseded(v2), 0);
    EXPECT_EQ(StatusOf(v2), DocStatus::kReady);
    const std::string v3 = Create("http_upload", "a.md", DocStatus::kProcessing, kDay3);
    EXPECT_EQ(store_->doc_withdraw_version(v3, 9200, "not ready"), 0);
    EXPECT_EQ(StatusOf(v3), DocStatus::kDeleted);
    EXPECT_EQ(Column(v3, "error_message"), "not ready");
    ASSERT_EQ(store_->doc_get_deleted_from_status(v3, &s), 0);
    EXPECT_FALSE(s.has_value());
}

// --- publication: the ready write and the retirement, in one transaction ------------

TEST_F(DocVersionsStoreTest, PublicationMarksReadyAndRetiresTogether) {
    const std::string v1 = Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    const std::string v2 = Create("http_upload", "a.md", DocStatus::kProcessing, kDay2);
    int64_t retired = -1;
    ASSERT_EQ(store_->doc_publish_version(v2, 7000, &retired), 0);
    EXPECT_EQ(retired, 1);
    EXPECT_EQ(StatusOf(v2), DocStatus::kReady);
    EXPECT_EQ(StatusOf(v1), DocStatus::kDeleted);
    EXPECT_EQ(Column(v1, "deleted_from_status"), "ready");
    EXPECT_EQ(Column(v1, "deleted_at"), "7000");
    EXPECT_FALSE(InTransaction());
}

TEST_F(DocVersionsStoreTest, OnlyUploadedAndWatchedVersionsRetireOlderRows) {
    const std::string f1 = Create("file", "f.md", DocStatus::kReady, kDay1);
    const std::string f2 = Create("file", "f.md", DocStatus::kProcessing, kDay2);
    int64_t retired = -1;
    ASSERT_EQ(store_->doc_publish_version(f2, 7000, &retired), 0);
    EXPECT_EQ(retired, 0);
    EXPECT_EQ(StatusOf(f2), DocStatus::kReady);
    EXPECT_EQ(StatusOf(f1), DocStatus::kReady);
    ASSERT_EQ(store_->doc_retire_older_versions(f2, 8000, &retired), 0);
    EXPECT_EQ(retired, 0);
    EXPECT_EQ(StatusOf(f1), DocStatus::kReady);
}

TEST_F(DocVersionsStoreTest, PublicationOfADeletedOrMissingRowChangesNothing) {
    const std::string v1 = Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    const std::string v2 = Create("http_upload", "a.md", DocStatus::kProcessing, kDay2);
    ASSERT_EQ(store_->doc_soft_delete(v2, 1000), 0);
    int64_t retired = -1;
    EXPECT_EQ(store_->doc_publish_version(v2, 7000, &retired), -2);
    EXPECT_EQ(retired, 0);
    EXPECT_FALSE(InTransaction());  // the transaction it began is over
    EXPECT_EQ(StatusOf(v2), DocStatus::kDeleted);
    EXPECT_EQ(StatusOf(v1), DocStatus::kReady);
    EXPECT_EQ(store_->doc_publish_version("no-such-doc", 7000, &retired), -2);
    EXPECT_FALSE(InTransaction());
}

TEST_F(DocVersionsStoreTest, FailedRetirementRollsBackTheReadyWrite) {
    const std::string v1 = Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    const std::string v2 = Create("http_upload", "a.md", DocStatus::kProcessing, kDay2);
    AbortWhen("abort_retire", v1, "deleted");
    int64_t retired = -1;
    EXPECT_EQ(store_->doc_publish_version(v2, 7000, &retired), -1);
    EXPECT_EQ(retired, 0);
    EXPECT_FALSE(InTransaction());
    EXPECT_EQ(StatusOf(v1), DocStatus::kReady);
    EXPECT_EQ(StatusOf(v2), DocStatus::kProcessing);
}

TEST_F(DocVersionsStoreTest, RefusedCommitRollsBackBothChanges) {
    const std::string v1 = Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    const std::string v2 = Create("http_upload", "a.md", DocStatus::kProcessing, kDay2);
    FailCommitWhen(v2, "ready");
    int64_t retired = -1;
    EXPECT_EQ(store_->doc_publish_version(v2, 7000, &retired), -1);
    EXPECT_EQ(retired, 0);
    EXPECT_FALSE(InTransaction());
    EXPECT_EQ(StatusOf(v1), DocStatus::kReady);
    EXPECT_EQ(StatusOf(v2), DocStatus::kProcessing);
}

TEST_F(DocVersionsStoreTest, PublicationInsideAnOpenTransactionLeavesItOpen) {
    Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    const std::string v2 = Create("http_upload", "a.md", DocStatus::kProcessing, kDay2);
    Exec("BEGIN");
    int64_t retired = -1;
    EXPECT_EQ(store_->doc_publish_version(v2, 7000, &retired), -1);
    EXPECT_TRUE(InTransaction());  // not ours to end
    Exec("ROLLBACK");
    EXPECT_EQ(StatusOf(v2), DocStatus::kProcessing);
}

// --- the restore that refuses beside a live version -----------------------------------

TEST_F(DocVersionsStoreTest, ConditionalRestoreRestoresOnlyWithoutALiveVersion) {
    const std::string v1 = Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    ASSERT_EQ(store_->doc_soft_delete(v1, 1000), 0);
    const std::string v2 = Create("http_upload", "a.md", DocStatus::kPending, kDay2);
    EXPECT_EQ(store_->doc_restore_unless_superseded(v1), -2);
    EXPECT_EQ(StatusOf(v1), DocStatus::kDeleted);
    EXPECT_EQ(Column(v1, "deleted_from_status"), "ready");

    // Live rows of another source do not refuse it.
    Create("watch_dir", "a.md", DocStatus::kReady, kDay3);
    Create("http_upload", "b.md", DocStatus::kReady, kDay3);
    ASSERT_EQ(store_->doc_soft_delete(v2, 2000), 0);
    EXPECT_EQ(store_->doc_restore_unless_superseded(v1), 0);
    EXPECT_EQ(StatusOf(v1), DocStatus::kReady);
    EXPECT_EQ(Column(v1, "deleted_from_status"), "<null>");
    EXPECT_EQ(Column(v1, "deleted_at"), "<null>");

    EXPECT_EQ(store_->doc_restore_unless_superseded(v1), -2);  // not deleted
    EXPECT_EQ(store_->doc_restore_unless_superseded("no-such-doc"), -2);
}

TEST_F(DocVersionsStoreTest, ConditionalRestoreCountsOnlyACommittedRestore) {
    const std::string v1 = Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    ASSERT_EQ(store_->doc_soft_delete(v1, 1000), 0);
    FailCommitWhen(v1, "ready");
    EXPECT_EQ(store_->doc_restore_unless_superseded(v1), -1);
    EXPECT_FALSE(InTransaction());
    EXPECT_EQ(StatusOf(v1), DocStatus::kDeleted);
}

// --- the withdraw of a version whose publication failed ------------------------------

TEST_F(DocVersionsStoreTest, WithdrawDeletesAFailedVersionWhileAnOlderOneIsLive) {
    const std::string v1 = Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    const std::string v2 = Create("http_upload", "a.md", DocStatus::kProcessing, kDay2);
    EXPECT_EQ(store_->doc_withdraw_version(v2, 7000, "not ready"), 0);
    EXPECT_EQ(StatusOf(v2), DocStatus::kDeleted);
    EXPECT_EQ(Column(v2, "deleted_from_status"), "processing");
    EXPECT_EQ(Column(v2, "deleted_at"), "7000");
    EXPECT_EQ(Column(v2, "error_message"), "not ready");
    EXPECT_EQ(StatusOf(v1), DocStatus::kReady);
    EXPECT_EQ(store_->doc_withdraw_version(v2, 8000, "again"), -2);  // already deleted
    EXPECT_EQ(Column(v2, "deleted_at"), "7000");

    // A pending version, beside an older version that is itself in error.
    const std::string w1 = Create("watch_dir", "/w/b.md", DocStatus::kError, kDay1);
    const std::string w2 = Create("watch_dir", "/w/b.md", DocStatus::kPending, kDay2);
    EXPECT_EQ(store_->doc_withdraw_version(w2, 7000, "not ready"), 0);
    EXPECT_EQ(Column(w2, "deleted_from_status"), "pending");
    EXPECT_EQ(StatusOf(w1), DocStatus::kError);
}

TEST_F(DocVersionsStoreTest, WithdrawLeavesEveryOtherRowAlone) {
    const std::string first = Create("http_upload", "first.md", DocStatus::kProcessing, kDay1);
    EXPECT_EQ(store_->doc_withdraw_version(first, 7000, "x"), -2);  // nothing older
    EXPECT_EQ(StatusOf(first), DocStatus::kProcessing);

    Create("http_upload", "r.md", DocStatus::kReady, kDay1);
    const std::string ready = Create("http_upload", "r.md", DocStatus::kReady, kDay2);
    EXPECT_EQ(store_->doc_withdraw_version(ready, 7000, "x"), -2);
    EXPECT_EQ(StatusOf(ready), DocStatus::kReady);

    Create("file", "f.md", DocStatus::kReady, kDay1);
    const std::string file = Create("file", "f.md", DocStatus::kProcessing, kDay2);
    EXPECT_EQ(store_->doc_withdraw_version(file, 7000, "x"), -2);
    EXPECT_EQ(StatusOf(file), DocStatus::kProcessing);

    const std::string gone = Create("http_upload", "d.md", DocStatus::kReady, kDay1);
    ASSERT_EQ(store_->doc_soft_delete(gone, 500), 0);
    const std::string after_gone = Create("http_upload", "d.md", DocStatus::kProcessing, kDay2);
    EXPECT_EQ(store_->doc_withdraw_version(after_gone, 7000, "x"), -2);

    const std::string older = Create("http_upload", "n.md", DocStatus::kProcessing, kDay1);
    Create("http_upload", "n.md", DocStatus::kPending, kDay2);  // only a newer one is live
    EXPECT_EQ(store_->doc_withdraw_version(older, 7000, "x"), -2);
    EXPECT_EQ(StatusOf(older), DocStatus::kProcessing);

    Create("watch_dir", "o.md", DocStatus::kReady, kDay1);  // the other versioned type
    const std::string other_type = Create("http_upload", "o.md", DocStatus::kProcessing, kDay2);
    EXPECT_EQ(store_->doc_withdraw_version(other_type, 7000, "x"), -2);

    Create("http_upload", "p1.md", DocStatus::kReady, kDay1);  // another path
    const std::string other_path = Create("http_upload", "p2.md", DocStatus::kProcessing, kDay2);
    EXPECT_EQ(store_->doc_withdraw_version(other_path, 7000, "x"), -2);
    EXPECT_EQ(StatusOf(other_path), DocStatus::kProcessing);

    EXPECT_EQ(store_->doc_withdraw_version("no-such-doc", 7000, "x"), -2);
}

TEST_F(DocVersionsStoreTest, WithdrawCountsOnlyACommittedChange) {
    Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    const std::string v2 = Create("http_upload", "a.md", DocStatus::kProcessing, kDay2);
    FailCommitWhen(v2, "deleted");
    EXPECT_EQ(store_->doc_withdraw_version(v2, 7000, "x"), -1);
    EXPECT_FALSE(InTransaction());
    EXPECT_EQ(StatusOf(v2), DocStatus::kProcessing);
}

TEST_F(DocVersionsStoreTest, WithdrawInsideAnOpenTransactionIsRefused) {
    Create("http_upload", "a.md", DocStatus::kReady, kDay1);
    const std::string v2 = Create("http_upload", "a.md", DocStatus::kProcessing, kDay2);
    Exec("BEGIN");
    EXPECT_EQ(store_->doc_withdraw_version(v2, 7000, "x"), -1);
    Exec("ROLLBACK");
    EXPECT_EQ(StatusOf(v2), DocStatus::kProcessing);
}

TEST(DocVersionsStoreProbeTest, ReadOnANullHandleIsAnError) {
    CortrixStoreSqlite store(static_cast<sqlite3*>(nullptr));
    std::optional<std::string> s;
    EXPECT_EQ(store.doc_get_deleted_from_status("any", &s), -1);
    EXPECT_FALSE(s.has_value());
}

}  // namespace
}  // namespace cortrix
