#include <gtest/gtest.h>
#include <sqlite3.h>

namespace {

// Initialize SQLite's global state before any test thread opens a connection.
// ctest runs one process per test; a concurrency test whose FIRST SQLite touch
// happens on parallel worker threads (per-facade private connections, namespace pool
// D-I1.bis) otherwise races SQLite's lazy bootstrap (sqlite3MutexInit) — the
// same invariant production pins by calling sqlite3_initialize() in main().
class SqliteInitEnvironment : public ::testing::Environment {
public:
    void SetUp() override { sqlite3_initialize(); }
};

const ::testing::Environment* const kSqliteInitEnv =
    ::testing::AddGlobalTestEnvironment(new SqliteInitEnvironment);

}  // namespace
