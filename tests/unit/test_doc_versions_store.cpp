// Store contracts for one row per document version (#157): the source lookup
// returns the newest live row, a status write never brings back a deleted row,
// older versions are retired by the newer one, and a soft delete records the
// status the row had so a restore can tell a finished version from one that never
// finished. Also covers a store created before documents gained that column.

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

    std::unique_ptr<CortrixStoreSqlite> store_;
};

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
}

TEST(DocVersionsStoreProbeTest, ReadOnANullHandleIsAnError) {
    CortrixStoreSqlite store(static_cast<sqlite3*>(nullptr));
    std::optional<std::string> s;
    EXPECT_EQ(store.doc_get_deleted_from_status("any", &s), -1);
    EXPECT_FALSE(s.has_value());
}

}  // namespace
}  // namespace cortrix
