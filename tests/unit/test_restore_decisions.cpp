// Restore decisions (#157): the order of the checks and every row of the decision
// table, over a real in-memory store, with injectable read failures. A restore never
// leaves two live versions of an uploaded or watched file, and never brings back as
// ready a version that never finished processing.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <optional>
#include <string>

#include "cortrix/catalog/gc/document_gc_sweeper.h"
#include "cortrix/server/routes/gc_routes.h"
#include "cortrix/store/cortrix_store_sqlite.h"

namespace cortrix::catalog::gc {
namespace {

using Kind = DocumentGcSweeper::RestoreDecision::Kind;

class FaultyStore : public CortrixStoreSqlite {
public:
    FaultyStore() : CortrixStoreSqlite(":memory:") {}
    bool fail_get = false;
    bool fail_marker = false;
    bool fail_lookup = false;
    bool fail_restore = false;
    int doc_get(const std::string& id, CortrixDoc& d) override {
        return fail_get ? -1 : CortrixStoreSqlite::doc_get(id, d);
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
        return fail_lookup ? -1 : CortrixStoreSqlite::doc_find_by_source(type, path, d);
    }
    int doc_restore(const std::string& id) override {
        return fail_restore ? -1 : CortrixStoreSqlite::doc_restore(id);
    }
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
