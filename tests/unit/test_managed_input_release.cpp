// Issue #74 -- releasing a managed input must be a point lookup on a resolved path
// identity, not a scan that resolves every live task's path on every completion.
//
// The properties that matter here are the ones the old scan provided and the new
// lookup has to keep: a file another live task still needs is never removed, two
// spellings of the same path are one identity, an unanswerable check keeps the file,
// and the decision is not separated from the removal. Those are asserted directly;
// the cost change is asserted structurally (the resolved identity is persisted and
// indexed, so the check cannot degrade back into a scan).
//
// Deterministic: temp-file TaskManager and a temp managed dir, no sleeps.

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cortrix/async/managed_input.h"
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/common/status.h"

namespace cortrix::async {
namespace {

namespace fs = std::filesystem;

class ManagedInputReleaseFx : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string tag =
            std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line());
        root_ = fs::temp_directory_path() / ("cortrix_managed_input_" + tag);
        fs::remove_all(root_);
        fs::create_directories(root_);
        managed_dir_ = (root_ / "batch_tmp").string();
        fs::create_directories(managed_dir_);
        db_path_ = (root_ / "tasks.db").string();
        ASSERT_TRUE(mgr_.Init(db_path_).ok());
    }

    void TearDown() override { fs::remove_all(root_); }

    std::string MakeFile(const std::string& name) {
        const fs::path p = fs::path(managed_dir_) / name;
        std::ofstream out(p);
        out << "content";
        out.close();
        return p.string();
    }

    // Creates a queued task owning `filepath` and returns its task_id.
    std::string CreateTaskFor(const std::string& filepath, const std::string& doc_id) {
        TaskInfo t;
        t.namespace_id = "ns";
        t.filename = "doc.pdf";
        t.filepath = filepath;
        t.doc_id = doc_id;
        t.content_hash = "h-" + doc_id;
        auto created = mgr_.CreateTask(t);
        EXPECT_TRUE(created.ok()) << created.status().message();
        return created.ok() ? created.value().task_id : std::string();
    }

    // queued -> processing -> completed: a task cannot reach a terminal state
    // without being claimed first.
    void Complete(const std::string& task_id, const std::string& doc_id) {
        ASSERT_TRUE(mgr_.MarkProcessing(task_id, /*worker_id=*/1).ok());
        ASSERT_TRUE(mgr_.MarkCompleted(task_id, doc_id).ok());
    }

    void Fail(const std::string& task_id) {
        ASSERT_TRUE(mgr_.MarkProcessing(task_id, /*worker_id=*/2).ok());
        ASSERT_TRUE(mgr_.MarkFailed(task_id, "CX_ERR_X", "gone").ok());
    }

    fs::path root_;
    std::string managed_dir_;
    std::string db_path_;
    TaskManager mgr_;
};

// ---- the file is released when nobody else needs it ----

TEST_F(ManagedInputReleaseFx, RemovesInputWhenNoOtherLiveTaskReferencesIt) {
    const std::string file = MakeFile("01ABC.pdf");
    const std::string owner = CreateTaskFor(file, "doc1");
    Complete(owner, "doc1");

    EXPECT_TRUE(ReleaseManagedPath(managed_dir_, file, owner, &mgr_));
    EXPECT_FALSE(fs::exists(file));
}

// ---- protection preserved ----

TEST_F(ManagedInputReleaseFx, KeepsInputWhileAnotherLiveTaskReferencesIt) {
    const std::string file = MakeFile("01SHARED.pdf");
    const std::string owner = CreateTaskFor(file, "doc1");
    CreateTaskFor(file, "doc2");  // second live task on the same input
    Complete(owner, "doc1");

    EXPECT_FALSE(ReleaseManagedPath(managed_dir_, file, owner, &mgr_));
    EXPECT_TRUE(fs::exists(file));
}

// The identity is the resolved path, not the stored string: a live task that spells
// the same file differently must still protect it.
TEST_F(ManagedInputReleaseFx, KeepsInputWhenOtherLiveTaskSpellsThePathDifferently) {
    const std::string file = MakeFile("01SPELL.pdf");
    const std::string owner = CreateTaskFor(file, "doc1");
    const std::string dotted =
        (fs::path(managed_dir_) / "." / "01SPELL.pdf").string();
    ASSERT_NE(dotted, file);
    CreateTaskFor(dotted, "doc2");
    Complete(owner, "doc1");

    EXPECT_FALSE(ReleaseManagedPath(managed_dir_, file, owner, &mgr_));
    EXPECT_TRUE(fs::exists(file));
}

// A terminal task no longer needs its input, so it must not block the release.
TEST_F(ManagedInputReleaseFx, TerminalTaskDoesNotBlockRelease) {
    const std::string file = MakeFile("01TERM.pdf");
    const std::string owner = CreateTaskFor(file, "doc1");
    const std::string other = CreateTaskFor(file, "doc2");
    Complete(owner, "doc1");
    Fail(other);

    EXPECT_TRUE(ReleaseManagedPath(managed_dir_, file, owner, &mgr_));
    EXPECT_FALSE(fs::exists(file));
}

// ---- fail closed ----

// A live row written before filepath_canonical existed carries NULL, so the point
// lookup cannot rule it out. The check must read that as "might be this file".
TEST_F(ManagedInputReleaseFx, KeepsInputWhenALiveRowHasUnresolvedIdentity) {
    const std::string file = MakeFile("01NULL.pdf");
    const std::string owner = CreateTaskFor(file, "doc1");
    CreateTaskFor(MakeFile("01OTHER.pdf"), "doc2");
    Complete(owner, "doc1");

    // Simulate the pre-migration row: blank the other live task's resolved identity.
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(db_path_.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw,
                           "UPDATE tasks SET filepath_canonical = NULL "
                           "WHERE doc_id = 'doc2'",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(raw);

    EXPECT_FALSE(ReleaseManagedPath(managed_dir_, file, owner, &mgr_));
    EXPECT_TRUE(fs::exists(file));
}

// Paths outside the managed dir are never touched, however the caller spells them.
TEST_F(ManagedInputReleaseFx, RefusesPathsOutsideTheManagedDir) {
    const fs::path outside = root_ / "not_managed.pdf";
    std::ofstream(outside) << "x";
    const std::string owner = CreateTaskFor(outside.string(), "doc1");
    Complete(owner, "doc1");

    EXPECT_FALSE(ReleaseManagedPath(managed_dir_, outside.string(), owner, &mgr_));
    EXPECT_TRUE(fs::exists(outside));

    const std::string escaping =
        (fs::path(managed_dir_) / ".." / "not_managed.pdf").string();
    EXPECT_FALSE(ReleaseManagedPath(managed_dir_, escaping, owner, &mgr_));
    EXPECT_TRUE(fs::exists(outside));
}

// ---- migration ----

// An existing database that predates the column gains it on the next Init, and its
// live rows get their identity resolved so the reaper can work on point lookups
// instead of permanently failing closed.
TEST_F(ManagedInputReleaseFx, BackfillResolvesLiveRowsOnReinit) {
    const std::string file = MakeFile("01BACKFILL.pdf");
    const std::string owner = CreateTaskFor(file, "doc1");
    const std::string live = CreateTaskFor(MakeFile("01LIVE.pdf"), "doc2");
    Complete(owner, "doc1");

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(db_path_.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "UPDATE tasks SET filepath_canonical = NULL", nullptr,
                           nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(raw);

    // Before the backfill the unresolved live row makes the check fail closed.
    EXPECT_FALSE(ReleaseManagedPath(managed_dir_, file, owner, &mgr_));
    EXPECT_TRUE(fs::exists(file));

    TaskManager reopened;
    ASSERT_TRUE(reopened.Init(db_path_).ok());
    EXPECT_TRUE(ReleaseManagedPath(managed_dir_, file, owner, &reopened));
    EXPECT_FALSE(fs::exists(file));
    EXPECT_TRUE(reopened.GetTask(live).ok());  // the live task is untouched
}

// A database written by a binary that predates filepath_canonical has the column
// missing, not merely NULL. Init must add it and still come up: indexing the column
// in the same batch as CREATE TABLE IF NOT EXISTS looks correct on a fresh database
// and fails on every existing one ("no such column"), taking the whole DDL batch --
// and Init -- down with it before the ALTER can run.
TEST_F(ManagedInputReleaseFx, InitUpgradesADatabaseWhoseTableLacksTheColumn) {
    const std::string legacy = (root_ / "legacy_tasks.db").string();
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(legacy.c_str(), &raw), SQLITE_OK);
    // The pre-#74 shape: every column except filepath_canonical.
    ASSERT_EQ(sqlite3_exec(raw,
                           "CREATE TABLE tasks ("
                           " task_id TEXT PRIMARY KEY, namespace_id TEXT NOT NULL,"
                           " filename TEXT NOT NULL, filepath TEXT NOT NULL, doc_id TEXT,"
                           " content_hash TEXT, status TEXT NOT NULL DEFAULT 'queued',"
                           " task_type INTEGER NOT NULL DEFAULT 1,"
                           " cancel_requested INTEGER NOT NULL DEFAULT 0,"
                           " total_pages INTEGER DEFAULT 0, processed_pages INTEGER DEFAULT 0,"
                           " failed_pages TEXT DEFAULT '[]', progress_pct REAL DEFAULT 0.0,"
                           " eta_seconds INTEGER DEFAULT -1, current_phase TEXT,"
                           " worker_id INTEGER, trace_id TEXT, error_code TEXT, error_msg TEXT,"
                           " structured_data TEXT, created_at TEXT NOT NULL,"
                           " updated_at TEXT NOT NULL, started_at TEXT, completed_at TEXT,"
                           " metadata_json TEXT)",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw,
                           "INSERT INTO tasks (task_id, namespace_id, filename, filepath,"
                           " doc_id, status, created_at, updated_at) VALUES"
                           " ('t-old','ns','doc.pdf','/tmp/old.pdf','doc-old','queued',"
                           "  '2026-01-01T00:00:00.000Z','2026-01-01T00:00:00.000Z')",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(raw);

    TaskManager upgraded;
    const Status init = upgraded.Init(legacy);
    ASSERT_TRUE(init.ok()) << init.message();

    // Column added, index built on it, and the pre-existing row still readable.
    ASSERT_EQ(sqlite3_open(legacy.c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  raw, "SELECT count(*) FROM pragma_table_info('tasks') "
                       "WHERE name='filepath_canonical'", -1, &stmt, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  raw, "SELECT count(*) FROM sqlite_master WHERE type='index' "
                       "AND name='idx_tasks_live_canonical'", -1, &stmt, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    sqlite3_close(raw);

    EXPECT_TRUE(upgraded.GetTask("t-old").ok());
}

// The resolved identity has to be persisted and indexed -- that is what keeps the
// reference check a point lookup rather than a per-completion scan.
TEST_F(ManagedInputReleaseFx, ResolvedIdentityIsPersistedAndIndexed) {
    const std::string dotted = (fs::path(managed_dir_) / "." / "01IDX.pdf").string();
    MakeFile("01IDX.pdf");
    CreateTaskFor(dotted, "doc1");

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(db_path_.c_str(), &raw), SQLITE_OK);

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  raw, "SELECT filepath, filepath_canonical FROM tasks WHERE doc_id='doc1'",
                  -1, &stmt, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const std::string stored = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string canonical = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    sqlite3_finalize(stmt);
    EXPECT_EQ(stored, dotted);                                   // raw spelling kept
    EXPECT_EQ(canonical, fs::weakly_canonical(dotted).string()); // resolved alongside

    sqlite3_stmt* plan = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw,
                                 "EXPLAIN QUERY PLAN SELECT 1 FROM tasks "
                                 "WHERE filepath_canonical = ? AND task_id <> ? "
                                 "AND status NOT IN ('completed','failed','cancelled') LIMIT 1",
                                 -1, &plan, nullptr),
              SQLITE_OK);
    std::string detail;
    while (sqlite3_step(plan) == SQLITE_ROW) {
        detail += reinterpret_cast<const char*>(sqlite3_column_text(plan, 3));
        detail += "\n";
    }
    sqlite3_finalize(plan);
    sqlite3_close(raw);
    EXPECT_NE(detail.find("idx_tasks_live_canonical"), std::string::npos) << detail;
    EXPECT_EQ(detail.find("SCAN tasks"), std::string::npos) << detail;
}

// The unknown-identity guard must stay cheap as history accumulates. Terminal rows
// keep a NULL identity forever -- they have already released their input and the
// backfill deliberately skips them -- so an index that covers them makes the guard
// walk every historical NULL before status can rule it out. Measured on a real
// 801k-row database: 1.109s per probe with a full index versus 0.003s with the
// live-only one, and the full-index cost grows with total history rather than with
// the live set. The plan is pinned against a table whose NULLs are overwhelmingly
// terminal, which is the shape that exposed it.
TEST_F(ManagedInputReleaseFx, UnknownIdentityGuardDoesNotWalkTerminalHistory) {
    CreateTaskFor(MakeFile("01LIVEONE.pdf"), "live1");

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(db_path_.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
    for (int i = 0; i < 2000; ++i) {
        const std::string sql =
            "INSERT INTO tasks (task_id, namespace_id, filename, filepath, status,"
            " created_at, updated_at, filepath_canonical) VALUES ('hist-" +
            std::to_string(i) + "','ns','d.pdf','/tmp/hist" + std::to_string(i) +
            ".pdf','completed','2026-01-01T00:00:00.000Z','2026-01-01T00:00:00.000Z',NULL)";
        ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }
    ASSERT_EQ(sqlite3_exec(raw, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);

    sqlite3_stmt* plan = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw,
                                 "EXPLAIN QUERY PLAN SELECT 1 FROM tasks "
                                 "WHERE filepath_canonical IS NULL "
                                 "AND filepath IS NOT NULL AND filepath <> '' AND task_id <> ? "
                                 "AND status NOT IN ('completed','failed','cancelled') LIMIT 1",
                                 -1, &plan, nullptr),
              SQLITE_OK);
    std::string detail;
    while (sqlite3_step(plan) == SQLITE_ROW) {
        detail += reinterpret_cast<const char*>(sqlite3_column_text(plan, 3));
        detail += "\n";
    }
    sqlite3_finalize(plan);

    // The index must be the live-only one; a full index over filepath_canonical
    // would also satisfy "uses an index" while walking all 2000 terminal NULLs.
    EXPECT_NE(detail.find("idx_tasks_live_canonical"), std::string::npos) << detail;
    EXPECT_EQ(detail.find("SCAN tasks"), std::string::npos) << detail;

    sqlite3_stmt* cnt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  raw, "SELECT count(*) FROM sqlite_master WHERE type='index' "
                       "AND name='idx_tasks_live_canonical' AND sql LIKE '%WHERE%status%'",
                  -1, &cnt, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(cnt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(cnt, 0), 1) << "index must be partial (live rows only)";
    sqlite3_finalize(cnt);
    sqlite3_close(raw);
}

}  // namespace
}  // namespace cortrix::async
