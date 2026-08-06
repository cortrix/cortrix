#include <gtest/gtest.h>

#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <utility>

#include "cortrix/common/status.h"
#include "cortrix/resource/namespace_pool_config.h"
#include "cortrix/resource/sqlite_conn.h"

// Branch coverage for SqliteConn (namespace pool store.db RAII owner). The happy
// ApplyPragmas/ReadPragma round-trip is exercised by NamespacePoolTest
// (UT17); this file drives the error + lifecycle branches: open failure,
// move semantics, no-op-when-closed guards, PRAGMA exec failure, and
// ReadPragma prepare/empty-result paths.
namespace cortrix::resource {
namespace {

class SqliteConnTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("sqlite_conn_branch_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path dir_;
};

// Opening under a path whose parent directory does not exist fails → the
// CX_ERR_NS_LOAD_FAILED-coded kUnavailable Status branch.
TEST_F(SqliteConnTest, OpenInvalidPathFailsWithLoadFailed) {
    SqliteConn conn;
    Status s = conn.Open((dir_ / "no-such-subdir" / "store.db").string());
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_NE(s.message().find("CX_ERR_NS_LOAD_FAILED"), std::string::npos);
    EXPECT_FALSE(conn.is_open());
}

TEST_F(SqliteConnTest, OpenSucceedsAndRecordsPath) {
    SqliteConn conn;
    const std::string path = (dir_ / "store.db").string();
    ASSERT_TRUE(conn.Open(path).ok());
    EXPECT_TRUE(conn.is_open());
    EXPECT_EQ(conn.path(), path);
    EXPECT_NE(conn.handle(), nullptr);
}

// Re-Open closes the prior handle first (the Close()-then-open branch).
TEST_F(SqliteConnTest, ReOpenClosesPrevious) {
    SqliteConn conn;
    ASSERT_TRUE(conn.Open((dir_ / "a.db").string()).ok());
    ASSERT_TRUE(conn.Open((dir_ / "b.db").string()).ok());
    EXPECT_EQ(conn.path(), (dir_ / "b.db").string());
}

// Move ctor transfers ownership and nulls the source.
TEST_F(SqliteConnTest, MoveConstructTransfersHandle) {
    SqliteConn a;
    ASSERT_TRUE(a.Open((dir_ / "m.db").string()).ok());
    sqlite3* h = a.handle();
    SqliteConn b(std::move(a));
    EXPECT_EQ(b.handle(), h);
    EXPECT_FALSE(a.is_open());  // NOLINT(bugprone-use-after-move) — intentional
}

// Move assignment closes the destination's existing handle, then transfers.
TEST_F(SqliteConnTest, MoveAssignClosesDestThenTransfers) {
    SqliteConn a;
    ASSERT_TRUE(a.Open((dir_ / "src.db").string()).ok());
    sqlite3* h = a.handle();
    SqliteConn b;
    ASSERT_TRUE(b.Open((dir_ / "dst.db").string()).ok());  // b already open
    b = std::move(a);
    EXPECT_EQ(b.handle(), h);
    EXPECT_FALSE(a.is_open());  // NOLINT(bugprone-use-after-move)
}

// Self move-assignment is a no-op (the `this != &other` guard branch).
TEST_F(SqliteConnTest, SelfMoveAssignIsNoOp) {
    SqliteConn a;
    ASSERT_TRUE(a.Open((dir_ / "self.db").string()).ok());
    sqlite3* h = a.handle();
    SqliteConn& ref = a;
    a = std::move(ref);  // NOLINT(clang-diagnostic-self-move) — exercises the guard
    EXPECT_EQ(a.handle(), h);
    EXPECT_TRUE(a.is_open());
}

// ApplyPragmas on a never-opened connection is a clean no-op Ok (db_ == nullptr).
TEST_F(SqliteConnTest, ApplyPragmasOnClosedConnIsOkNoOp) {
    SqliteConn conn;
    SqlitePragmas p;
    EXPECT_TRUE(conn.ApplyPragmas(p).ok());
}

// A PRAGMA whose value carries trailing SQL makes sqlite3_exec hit a syntax error
// on the appended statement → the first-error branch surfaces a kInternal Status,
// while the connection stays usable (failure is tolerated).
TEST_F(SqliteConnTest, ApplyPragmasSurfacesFirstFailureButStaysUsable) {
    SqliteConn conn;
    ASSERT_TRUE(conn.Open((dir_ / "store.db").string()).ok());
    SqlitePragmas p;
    // Make TWO pragmas a hard parse error. An unterminated single-quote yields
    // "unrecognized token" from sqlite3_prepare inside sqlite3_exec (SQLite is too
    // lenient about bad *values* like "=X" — only a true token error fails). The
    // assembled statements are PRAGMA journal_mode='bad  and  PRAGMA temp_store='bad.
    // This covers both operands of `rc != SQLITE_OK && first_error.ok()`:
    //   - journal_mode fails → rc!=OK true, first_error.ok() true  → first_error set
    //   - temp_store fails    → rc!=OK true, first_error.ok() FALSE → second-operand
    //     false arm (the surface keeps the FIRST failure only).
    p.journal_mode = "'bad";
    p.temp_store = "'bad";
    Status s = conn.ApplyPragmas(p);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("PRAGMA failed"), std::string::npos);
    // The surfaced failure is the FIRST one (journal_mode), proving first_error was
    // not overwritten by the later temp_store failure (the first_error.ok()==false arm).
    EXPECT_NE(s.message().find("journal_mode"), std::string::npos) << s.message();
    // Connection survives — a subsequent valid read still works.
    EXPECT_TRUE(conn.ReadPragma("journal_mode").ok());
}

// A single PRAGMA failure (only one bad value) takes the first-failure arm with no
// later iteration overwriting it — isolates one rc!=OK true / first_error.ok() true
// pass with every other pragma succeeding.
TEST_F(SqliteConnTest, ApplyPragmasSingleFailureSurfacesIt) {
    SqliteConn conn;
    ASSERT_TRUE(conn.Open((dir_ / "store.db").string()).ok());
    SqlitePragmas p;
    p.synchronous = "'nope";  // only this one is a hard parse error
    Status s = conn.ApplyPragmas(p);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("synchronous"), std::string::npos) << s.message();
}

// ReadPragma on a closed connection returns a kInternal Status (db_ == nullptr).
TEST_F(SqliteConnTest, ReadPragmaOnClosedConnIsInternal) {
    SqliteConn conn;
    auto r = conn.ReadPragma("journal_mode");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
}

// A malformed pragma name makes sqlite3_prepare_v2 fail → the prepare-error branch.
TEST_F(SqliteConnTest, ReadPragmaPrepareFailureIsInternal) {
    SqliteConn conn;
    ASSERT_TRUE(conn.Open((dir_ / "store.db").string()).ok());
    auto r = conn.ReadPragma("not valid; pragma name");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
    EXPECT_NE(r.status().message().find("ReadPragma prepare failed"), std::string::npos);
}

// A PRAGMA that yields no row (e.g. setting-style with no output) leaves the
// returned value empty — the "step != SQLITE_ROW" branch.
TEST_F(SqliteConnTest, ReadPragmaWithNoRowReturnsEmpty) {
    SqliteConn conn;
    ASSERT_TRUE(conn.Open((dir_ / "store.db").string()).ok());
    // shrink_memory is a pure action pragma that emits no result row; ReadPragma
    // falls through the "step != SQLITE_ROW" path with an empty value.
    auto r = conn.ReadPragma("shrink_memory");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

}  // namespace
}  // namespace cortrix::resource
