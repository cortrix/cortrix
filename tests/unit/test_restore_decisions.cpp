// Restore decisions (#157): the order of the checks and every row of the decision
// table, over a real in-memory store, with injectable read failures. A restore never
// leaves two live versions of an uploaded or watched file, and never brings back as
// ready a version that never finished processing.

#include <gtest/gtest.h>

#include <sqlite3.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "cortrix/catalog/gc/document_gc_sweeper.h"
#include "cortrix/server/routes/gc_routes.h"
#include "cortrix/store/cortrix_store_sqlite.h"

namespace cortrix::catalog::gc {
namespace {

using Kind = DocumentGcSweeper::RestoreDecision::Kind;

class FaultyStore : public CortrixStoreSqlite {
public:
    explicit FaultyStore(const std::string& path = ":memory:") : CortrixStoreSqlite(path) {}
    bool fail_get = false;
    bool fail_marker = false;
    bool fail_lookup = false;
    bool fail_restore = false;  // both restores: the plain one and the conditional one
    // Each hook runs once, just before the base call: another writer finishing between
    // two of the decision's steps.
    std::function<void()> before_lookup;
    std::function<void()> before_restore;
    std::function<void()> before_conditional_restore;
    // Fail only the n-th doc_get from now (1 is the next call).
    void FailGetCall(int n) { fail_get_call_ = get_calls_ + n; }

    int doc_get(const std::string& id, CortrixDoc& d) override {
        ++get_calls_;
        if (fail_get || get_calls_ == fail_get_call_) return -1;
        return CortrixStoreSqlite::doc_get(id, d);
    }
    int doc_get_deleted_from_status(const std::string& id,
                                    std::optional<std::string>* out) override {
        if (fail_marker) {
            if (out) out->reset();
            return -1;
        }
        return CortrixStoreSqlite::doc_get_deleted_from_status(id, out);
    }
    int doc_find_by_source(const std::string& type, const std::string& path,
                           CortrixDoc& d) override {
        RunOnce(before_lookup);
        return fail_lookup ? -1 : CortrixStoreSqlite::doc_find_by_source(type, path, d);
    }
    int doc_restore(const std::string& id) override {
        RunOnce(before_restore);
        return fail_restore ? -1 : CortrixStoreSqlite::doc_restore(id);
    }
    int doc_restore_unless_superseded(const std::string& id) override {
        RunOnce(before_conditional_restore);
        return fail_restore ? -1 : CortrixStoreSqlite::doc_restore_unless_superseded(id);
    }

private:
    static void RunOnce(std::function<void()>& hook) {
        if (!hook) return;
        auto run = std::move(hook);
        hook = nullptr;
        run();
    }
    int get_calls_ = 0;
    int fail_get_call_ = 0;
};

class RestoreDecisionTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(store_.Open(), 0); }

    std::string Row(const std::string& type, const std::string& path, DocStatus status,
                    const std::string& created_at) {
        CortrixDoc d;
        d.source_type = type;
        d.source_path = path;
        d.status = status;
        EXPECT_EQ(store_.doc_create(d), 0);
        char* err = nullptr;
        const std::string sql = "UPDATE documents SET created_at = '" + created_at +
                                "' WHERE doc_id = '" + d.doc_id + "'";
        EXPECT_EQ(sqlite3_exec(store_.db_handle(), sql.c_str(), nullptr, nullptr, &err),
                  SQLITE_OK);
        sqlite3_free(err);
        return d.doc_id;
    }

    // Soft-deleted from `from`, with that status recorded.
    std::string DeletedFrom(const std::string& type, const std::string& path, DocStatus from,
                            const std::string& created_at = "2026-10-01T00:00:00.000Z") {
        const std::string id = Row(type, path, from, created_at);
        EXPECT_EQ(store_.doc_soft_delete(id, 1000), 0);
        return id;
    }

    // Deleted without a recorded status, as a row deleted before the store recorded it.
    std::string DeletedUnknown(const std::string& type, const std::string& path) {
        const std::string id = Row(type, path, DocStatus::kReady, "2026-10-01T00:00:00.000Z");
        EXPECT_EQ(store_.doc_update_status(id, DocStatus::kDeleted), 0);
        std::optional<std::string> s;
        EXPECT_EQ(store_.doc_get_deleted_from_status(id, &s), 0);
        EXPECT_FALSE(s.has_value());
        return id;
    }

    std::string Live(const std::string& type, const std::string& path,
                     DocStatus status = DocStatus::kReady) {
        return Row(type, path, status, "2026-10-05T00:00:00.000Z");
    }

    DocumentGcSweeper::RestoreDecision Decide(const std::string& id) {
        return DocumentGcSweeper::DecideRestore(store_, id, "ns-1");
    }

    DocStatus StatusOf(const std::string& id) {
        CortrixDoc d;
        EXPECT_EQ(store_.doc_get(id, d), 0);
        return d.status;
    }

    FaultyStore store_;
};

void ExpectSuperseded(const DocumentGcSweeper::RestoreDecision& d, const std::string& id,
                      const std::string& live) {
    ASSERT_EQ(d.kind, Kind::kRefused);
    EXPECT_EQ(d.failure.id, id);
    EXPECT_EQ(d.failure.code, "CX_ERR_GC_RESTORE_SUPERSEDED");
    EXPECT_EQ(d.failure.live_doc_id, live);
    EXPECT_EQ(d.failure.namespace_name, "ns-1");
    EXPECT_EQ(d.failure.message, "another version is live: delete " + live +
                                     " in namespace ns-1, then restore " + id);
}

void ExpectIncomplete(const DocumentGcSweeper::RestoreDecision& d, const std::string& id) {
    ASSERT_EQ(d.kind, Kind::kRefused);
    EXPECT_EQ(d.failure.id, id);
    EXPECT_EQ(d.failure.code, "CX_ERR_GC_RESTORE_INCOMPLETE");
    EXPECT_EQ(d.failure.namespace_name, "ns-1");
    EXPECT_TRUE(d.failure.live_doc_id.empty());
    EXPECT_EQ(d.failure.message,
              "this version never finished processing; add the document again");
}

void ExpectCheckFailed(const DocumentGcSweeper::RestoreDecision& d) {
    ASSERT_EQ(d.kind, Kind::kRefused);
    EXPECT_EQ(d.failure.code, "CX_ERR_GC_RESTORE_CHECK_FAILED");
    EXPECT_EQ(d.failure.message, "could not check this document's versions; retry");
}

// --- unknown status (deleted before the store recorded it) ----------------------

TEST_F(RestoreDecisionTest, UnknownVersionedRowWithALiveVersionIsSuperseded) {
    const std::string id = DeletedUnknown("http_upload", "a.txt");
    const std::string live = Live("http_upload", "a.txt");
    ExpectSuperseded(Decide(id), id, live);
    EXPECT_EQ(StatusOf(id), DocStatus::kDeleted);
}

TEST_F(RestoreDecisionTest, UnknownVersionedRowAloneIsRestored) {
    const std::string id = DeletedUnknown("watch_dir", "/w/a.txt");
    EXPECT_EQ(Decide(id).kind, Kind::kRestored);
    EXPECT_EQ(StatusOf(id), DocStatus::kReady);
}

TEST_F(RestoreDecisionTest, UnknownSessionAndOtherRowsAreRestored) {
    const std::string session = DeletedUnknown("memory_session", "s-1");
    Live("memory_session", "s-1");
    EXPECT_EQ(Decide(session).kind, Kind::kRestored);
    const std::string file = DeletedUnknown("file", "f.txt");
    Live("file", "f.txt");
    EXPECT_EQ(Decide(file).kind, Kind::kRestored);
}

// --- deleted from ready ------------------------------------------------------------

TEST_F(RestoreDecisionTest, ReadyVersionedRowWithALiveVersionIsSuperseded) {
    const std::string id = DeletedFrom("http_upload", "a.txt", DocStatus::kReady);
    const std::string live = Live("http_upload", "a.txt", DocStatus::kPending);
    ExpectSuperseded(Decide(id), id, live);
}

TEST_F(RestoreDecisionTest, ReadyVersionedRowAloneIsRestored) {
    const std::string id = DeletedFrom("http_upload", "a.txt", DocStatus::kReady);
    EXPECT_EQ(Decide(id).kind, Kind::kRestored);
    EXPECT_EQ(StatusOf(id), DocStatus::kReady);
}

TEST_F(RestoreDecisionTest, ReadySessionAndFileRowsAreRestoredBesideALiveRow) {
    const std::string session = DeletedFrom("memory_session", "s-1", DocStatus::kReady);
    Live("memory_session", "s-1");  // the next turn's new session document
    EXPECT_EQ(Decide(session).kind, Kind::kRestored);
    const std::string file = DeletedFrom("file", "f.txt", DocStatus::kReady);
    Live("file", "f.txt");
    EXPECT_EQ(Decide(file).kind, Kind::kRestored);
}

// --- deleted before it finished ------------------------------------------------------

TEST_F(RestoreDecisionTest, UnfinishedVersionWithALiveVersionIsIncompleteNotSuperseded) {
    // The incomplete check comes first: deleting the live version, as a superseded
    // refusal would ask, could never make this one restorable.
    const std::string id = DeletedFrom("http_upload", "a.txt", DocStatus::kProcessing);
    Live("http_upload", "a.txt");
    ExpectIncomplete(Decide(id), id);
}

TEST_F(RestoreDecisionTest, UnfinishedVersionAloneIsIncomplete) {
    for (DocStatus from : {DocStatus::kPending, DocStatus::kProcessing, DocStatus::kError,
                           DocStatus::kStale, DocStatus::kDeleting}) {
        const std::string id = DeletedFrom("watch_dir", "/w/" + std::to_string(static_cast<int>(from)),
                                           from);
        ExpectIncomplete(Decide(id), id);
        EXPECT_EQ(StatusOf(id), DocStatus::kDeleted);
    }
}

TEST_F(RestoreDecisionTest, UnfinishedFileIsIncomplete) {
    const std::string id = DeletedFrom("file", "f.txt", DocStatus::kPending);
    ExpectIncomplete(Decide(id), id);
}

TEST_F(RestoreDecisionTest, SessionDeletedAfterAFailedTurnIsRestoredBesideALiveSibling) {
    // A session document's status describes only its last turn.
    const std::string failed_turn = DeletedFrom("memory_session", "s-2", DocStatus::kError);
    Live("memory_session", "s-2");
    EXPECT_EQ(Decide(failed_turn).kind, Kind::kRestored);
}

// --- check failures, not here, and an older live version --------------------------------

TEST_F(RestoreDecisionTest, EachFailedReadIsACheckFailure) {
    const std::string id = DeletedFrom("http_upload", "a.txt", DocStatus::kReady);
    // A live version makes the restore refuse, so the lookup that names it runs too.
    const std::string live = Live("http_upload", "a.txt");
    store_.fail_get = true;
    ExpectCheckFailed(Decide(id));
    store_.fail_get = false;
    store_.fail_marker = true;
    ExpectCheckFailed(Decide(id));
    store_.fail_marker = false;
    store_.fail_lookup = true;
    ExpectCheckFailed(Decide(id));
    store_.fail_lookup = false;
    store_.fail_restore = true;
    ExpectCheckFailed(Decide(id));
    store_.fail_restore = false;
    EXPECT_EQ(StatusOf(id), DocStatus::kDeleted);
    ASSERT_EQ(store_.doc_soft_delete(live, 2000), 0);
    EXPECT_EQ(Decide(id).kind, Kind::kRestored);
}

TEST_F(RestoreDecisionTest, AFailedReadAfterTheRefusalIsACheckFailure) {
    const std::string id = DeletedFrom("http_upload", "a.txt", DocStatus::kReady);
    Live("http_upload", "a.txt");
    store_.FailGetCall(2);  // the first read passes; the one after the refusal fails
    ExpectCheckFailed(Decide(id));
    EXPECT_EQ(StatusOf(id), DocStatus::kDeleted);
}

TEST_F(RestoreDecisionTest, ALiveVersionGoneBeforeItIsNamedIsACheckFailure) {
    // The restore refuses because of `live`, which is deleted before the lookup can
    // name it: neither answer is certain, and a retry decides.
    const std::string id = DeletedFrom("http_upload", "a.txt", DocStatus::kReady);
    const std::string live = Live("http_upload", "a.txt");
    store_.before_lookup = [this, live] {
        ASSERT_EQ(store_.doc_soft_delete(live, 2000), 0);
    };
    ExpectCheckFailed(Decide(id));
    EXPECT_EQ(StatusOf(id), DocStatus::kDeleted);
    EXPECT_EQ(Decide(id).kind, Kind::kRestored);
}

TEST_F(RestoreDecisionTest, AMissingOrLiveRowIsNotHere) {
    EXPECT_EQ(Decide("no-such-doc").kind, Kind::kNotHere);
    const std::string live = Live("http_upload", "a.txt");
    EXPECT_EQ(Decide(live).kind, Kind::kNotHere);
}

TEST_F(RestoreDecisionTest, TheLiveVersionNamedCanBeOlderThanTheOneRestored) {
    const std::string v1 = Row("http_upload", "a.txt", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    const std::string v2 = Row("http_upload", "a.txt", DocStatus::kReady, "2026-10-02T00:00:00.000Z");
    int64_t retired = 0;
    ASSERT_EQ(store_.doc_retire_older_versions(v2, 2000, &retired), 0);
    ASSERT_EQ(retired, 1);
    ASSERT_EQ(store_.doc_soft_delete(v2, 3000), 0);
    EXPECT_EQ(Decide(v1).kind, Kind::kRestored);  // nothing is live
    ExpectSuperseded(Decide(v2), v2, v1);         // v1, older, is live now
}

// --- an upload committed by another connection while the restore is decided ---------

class RestoreInterleavingTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("cortrix_restore_race_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir_);
        const std::string path = (dir_ / "store.db").string();
        store_ = std::make_unique<FaultyStore>(path);
        ASSERT_EQ(store_->Open(), 0);
        other_ = std::make_unique<CortrixStoreSqlite>(path);  // an upload's connection
        ASSERT_EQ(other_->Open(), 0);
    }
    void TearDown() override {
        other_.reset();
        store_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    int64_t LiveRows(const std::string& type, const std::string& path) {
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(store_->db_handle(),
                                     "SELECT COUNT(*) FROM documents WHERE source_type = ? AND "
                                     "source_path = ? AND status != 'deleted'",
                                     -1, &st, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(st, 1, type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        int64_t n = -1;
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return n;
    }

    std::filesystem::path dir_;
    std::unique_ptr<FaultyStore> store_;
    std::unique_ptr<CortrixStoreSqlite> other_;
};

TEST_F(RestoreInterleavingTest, AnUploadFinishedJustBeforeTheRestoreRefusesIt) {
    CortrixDoc v1;
    v1.source_type = "http_upload";
    v1.source_path = "a.txt";
    v1.status = DocStatus::kReady;
    ASSERT_EQ(store_->doc_create(v1), 0);
    ASSERT_EQ(store_->doc_soft_delete(v1.doc_id, 1000), 0);

    // The other connection commits a live v2 just before the restore writes: before the
    // conditional statement, or, for a restore that checks and then writes, between the
    // two.
    std::string v2;
    auto upload_v2 = [this, &v2] {
        if (!v2.empty()) return;
        CortrixDoc d;
        d.source_type = "http_upload";
        d.source_path = "a.txt";
        d.status = DocStatus::kReady;
        ASSERT_EQ(other_->doc_create(d), 0);
        v2 = d.doc_id;
    };
    store_->before_conditional_restore = upload_v2;
    store_->before_restore = upload_v2;

    const auto d = DocumentGcSweeper::DecideRestore(*store_, v1.doc_id, "ns-1");
    ASSERT_FALSE(v2.empty()) << "the other connection never wrote";
    ExpectSuperseded(d, v1.doc_id, v2);
    CortrixDoc row;
    ASSERT_EQ(store_->doc_get(v1.doc_id, row), 0);
    EXPECT_EQ(row.status, DocStatus::kDeleted);
    EXPECT_EQ(LiveRows("http_upload", "a.txt"), 1);
}

TEST(GcRestoreFailureTraitsTest, OnlyAFailedCheckIsRetryable) {
    const auto check = GcRestoreFailureTraitsFor("CX_ERR_GC_RESTORE_CHECK_FAILED");
    EXPECT_TRUE(check.retryable);
    EXPECT_STREQ(check.category, "transient");
    for (const char* code : {"CX_ERR_GC_RESTORE_SUPERSEDED", "CX_ERR_GC_RESTORE_INCOMPLETE",
                             "CX_ERR_GC_DOC_NOT_FOUND"}) {
        const auto t = GcRestoreFailureTraitsFor(code);
        EXPECT_FALSE(t.retryable) << code;
        EXPECT_STREQ(t.category, "permanent") << code;
    }
}

}  // namespace
}  // namespace cortrix::catalog::gc
