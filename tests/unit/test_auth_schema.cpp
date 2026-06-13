#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "cortrix/auth/auth_schema.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/catalog/schema_provider.h"

// P08 S1 coverage: the 7-table platform.db schema + idempotency (P08 §3 / §S1
// `Schema_Create7Tables`, `Schema_Idempotent`).
namespace cortrix::auth {
namespace {

bool TableExists(sqlite3* db, const std::string& table) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
                  db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1,
                  &stmt, nullptr),
              SQLITE_OK)
        << sqlite3_errmsg(db);
    bool found = false;
    if (stmt) {
        sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
        found = (sqlite3_step(stmt) == SQLITE_ROW);
    }
    sqlite3_finalize(stmt);
    return found;
}

bool IndexExists(sqlite3* db, const std::string& index) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
                  db, "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?", -1,
                  &stmt, nullptr),
              SQLITE_OK)
        << sqlite3_errmsg(db);
    bool found = false;
    if (stmt) {
        sqlite3_bind_text(stmt, 1, index.c_str(), -1, SQLITE_TRANSIENT);
        found = (sqlite3_step(stmt) == SQLITE_ROW);
    }
    sqlite3_finalize(stmt);
    return found;
}

const char* const kAll7Tables[] = {
    "users", "refresh_tokens", "verification_codes", "token_blacklist",
    "auth_secrets", "auth_config", "api_keys",
};

// `Schema_Create7Tables`: empty platform.db → 7 tables + indexes present.
TEST(AuthSchemaTest, Create7Tables) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    sqlite3* db = pdb.db();
    ASSERT_NE(db, nullptr);

    for (const char* t : kAll7Tables) {
        EXPECT_TRUE(TableExists(db, t)) << "missing table: " << t;
    }
    // schema_version records P08 at v1.
    EXPECT_EQ(catalog::SchemaMigrator().CurrentVersion(db, "P08"), kP08AuthSchemaVersion);
}

TEST(AuthSchemaTest, AllIndexesCreated) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    sqlite3* db = pdb.db();
    for (const char* idx : {"idx_users_email", "idx_refresh_tokens_user",
                            "idx_refresh_tokens_expires",
                            "idx_verification_codes_email_type",
                            "idx_token_blacklist_expires",
                            "idx_auth_secrets_type_status",
                            "idx_api_keys_user_status", "idx_api_keys_key_hash"}) {
        EXPECT_TRUE(IndexExists(db, idx)) << "missing index: " << idx;
    }
}

// `Schema_Idempotent`: two consecutive migrations do not error (IF NOT EXISTS +
// the migrator's schema_version gate). Use an on-disk file so the second Open
// re-reads a persisted schema_version row.
TEST(AuthSchemaTest, Idempotent) {
    const std::string path = std::string(::testing::TempDir()) + "p08_idempotent.db";
    std::remove(path.c_str());

    {
        PlatformDb p1;
        ASSERT_TRUE(p1.Open(path).ok());
        EXPECT_TRUE(TableExists(p1.db(), "users"));
    }
    {
        PlatformDb p2;  // second migration on the same file: no-op, still ok.
        Status st = p2.Open(path);
        ASSERT_TRUE(st.ok()) << st.message();
        for (const char* t : kAll7Tables) EXPECT_TRUE(TableExists(p2.db(), t));
        EXPECT_EQ(catalog::SchemaMigrator().CurrentVersion(p2.db(), "P08"),
                  kP08AuthSchemaVersion);
    }
    std::remove(path.c_str());
}

// The §3.5 CHECK constraints are real: secret_type must be 'jwt_secret' and
// status must be in the enum. Proves the DDL carries the constraints (defensive
// against a silently-dropped CHECK during a future edit).
TEST(AuthSchemaTest, AuthSecretsCheckConstraints) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    sqlite3* db = pdb.db();

    // Bad secret_type → CHECK fails.
    EXPECT_NE(sqlite3_exec(
                  db,
                  "INSERT INTO auth_secrets(id, secret_type, value, status, created_at) "
                  "VALUES('x','other',X'00','current',1)",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    // Bad status → CHECK fails.
    EXPECT_NE(sqlite3_exec(
                  db,
                  "INSERT INTO auth_secrets(id, secret_type, value, status, created_at) "
                  "VALUES('x','jwt_secret',X'00','bogus',1)",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    // Valid row → OK.
    EXPECT_EQ(sqlite3_exec(
                  db,
                  "INSERT INTO auth_secrets(id, secret_type, value, status, created_at) "
                  "VALUES('x','jwt_secret',X'00','current',1)",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
}

// DB-separation guard (auth_schema.h): a freshly-opened platform.db has the P08
// `users` columns (password_hash / status / login_attempts), NOT the thin
// catalog `users` (user_id / email / created_at only). This is what keeps the
// same-name-different-file tables from being confused.
TEST(AuthSchemaTest, PlatformUsersHasAuthColumns) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    sqlite3* db = pdb.db();

    std::vector<std::string> cols;
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA table_info(users)", -1, &stmt, nullptr),
              SQLITE_OK);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cols.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);

    auto has = [&](const std::string& c) {
        return std::find(cols.begin(), cols.end(), c) != cols.end();
    };
    EXPECT_TRUE(has("password_hash"));   // P08 users, not catalog users
    EXPECT_TRUE(has("login_attempts"));
    EXPECT_TRUE(has("display_name"));
    // catalog `users` PK column is `user_id`; P08 `users` PK is `id`.
    EXPECT_TRUE(has("id"));
    EXPECT_FALSE(has("user_id"));
}

// Extra-provider registration: P08 runs first, an extra provider (FK→users)
// migrates after it in one atomic batch. Mirrors the catalog multi-provider
// path; proves platform.db reuses the frozen SchemaMigrator scaffold correctly.
class FakeProvider : public catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "FAKE"; }
    int CurrentVersion() const override { return 1; }
    Status Migrate(sqlite3* db, int, int) override {
        // FK→users(id) only succeeds if P08 already created `users`.
        const char* ddl =
            "CREATE TABLE feat_fake (id INTEGER PRIMARY KEY, "
            "uid TEXT REFERENCES users(id))";
        char* err = nullptr;
        if (sqlite3_exec(db, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string m = err ? err : "err";
            sqlite3_free(err);
            return Status::Internal(m);
        }
        return Status::Ok();
    }
};

TEST(AuthSchemaTest, ExtraProviderRunsAfterP08) {
    FakeProvider fake;
    PlatformDb pdb;
    Status st = pdb.Open(":memory:", {&fake});
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(TableExists(pdb.db(), "users"));      // P08 base
    EXPECT_TRUE(TableExists(pdb.db(), "feat_fake"));  // extra (FK→users)
}

}  // namespace
}  // namespace cortrix::auth
