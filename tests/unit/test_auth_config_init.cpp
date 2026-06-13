#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "cortrix/auth/auth_config_service.h"
#include "cortrix/auth/jwt_secret_service.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/config/auth_config.h"

// P08 S1 coverage: JwtSecretService::LoadOrInit + AuthConfigService::
// LoadOrInitDefaults (P08 §S1 steps 3-5 / §2.11 / §2.12 / §3.6).
namespace cortrix::auth {
namespace {

int RowCount(sqlite3* db, const char* table) {
    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK);
    int n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

// `JwtSecret_AutoGenerate_FirstStart`: fresh platform.db → LoadOrInit writes one
// status='current' 64-byte secret.
TEST(JwtSecretServiceTest, AutoGenerateFirstStart) {
    ::unsetenv("CORTRIX_JWT_SECRET");
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());

    JwtSecretService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInit().ok());
    EXPECT_TRUE(svc.initialized());
    EXPECT_EQ(svc.current_secret().size(), 64u);
    EXPECT_EQ(RowCount(pdb.db(), "auth_secrets"), 1);

    // Exactly one 'current' jwt_secret row.
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  pdb.db(),
                  "SELECT COUNT(*) FROM auth_secrets "
                  "WHERE secret_type='jwt_secret' AND status='current'",
                  -1, &stmt, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
}

// `JwtSecret_LoadExisting_AcrossRestart`: a second service over the same on-disk
// db loads the SAME secret, does NOT generate a new row.
TEST(JwtSecretServiceTest, LoadExistingAcrossRestart) {
    ::unsetenv("CORTRIX_JWT_SECRET");
    const std::string path = std::string(::testing::TempDir()) + "p08_jwt_restart.db";
    std::remove(path.c_str());

    std::string first;
    {
        PlatformDb p1;
        ASSERT_TRUE(p1.Open(path).ok());
        JwtSecretService s1(p1.db());
        ASSERT_TRUE(s1.LoadOrInit().ok());
        first = s1.current_secret();
        EXPECT_EQ(RowCount(p1.db(), "auth_secrets"), 1);
    }
    {
        PlatformDb p2;
        ASSERT_TRUE(p2.Open(path).ok());
        JwtSecretService s2(p2.db());
        ASSERT_TRUE(s2.LoadOrInit().ok());
        EXPECT_EQ(s2.current_secret(), first);          // same secret
        EXPECT_EQ(RowCount(p2.db(), "auth_secrets"), 1);  // no new row
    }
    std::remove(path.c_str());
}

// `JwtSecret_EnvOverride`: CORTRIX_JWT_SECRET set → that value is used + written
// through as the sole current secret (advanced K8s/docker path, §2.11 step 1).
TEST(JwtSecretServiceTest, EnvOverride) {
    const std::string path = std::string(::testing::TempDir()) + "p08_jwt_env.db";
    std::remove(path.c_str());

    // Seed an auto-generated secret first, then restart WITH the env set: the env
    // value must win and the old current be demoted (not stay 'current').
    {
        ::unsetenv("CORTRIX_JWT_SECRET");
        PlatformDb p0;
        ASSERT_TRUE(p0.Open(path).ok());
        JwtSecretService s0(p0.db());
        ASSERT_TRUE(s0.LoadOrInit().ok());
    }

    const std::string env_secret = "env-provided-secret-value-1234567890";
    ::setenv("CORTRIX_JWT_SECRET", env_secret.c_str(), /*overwrite=*/1);
    {
        PlatformDb p1;
        ASSERT_TRUE(p1.Open(path).ok());
        JwtSecretService s1(p1.db());
        ASSERT_TRUE(s1.LoadOrInit().ok());
        EXPECT_EQ(s1.current_secret(), env_secret);

        // Exactly one current row, and its value is the env secret.
        sqlite3_stmt* stmt = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(
                      p1.db(),
                      "SELECT value FROM auth_secrets "
                      "WHERE secret_type='jwt_secret' AND status='current'",
                      -1, &stmt, nullptr),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        const void* blob = sqlite3_column_blob(stmt, 0);
        const int len = sqlite3_column_bytes(stmt, 0);
        EXPECT_EQ(std::string(static_cast<const char*>(blob), len), env_secret);
        EXPECT_EQ(sqlite3_step(stmt), SQLITE_DONE);  // only one current row
        sqlite3_finalize(stmt);
    }
    ::unsetenv("CORTRIX_JWT_SECRET");
    std::remove(path.c_str());
}

TEST(JwtSecretServiceTest, NullDbHandleFails) {
    ::unsetenv("CORTRIX_JWT_SECRET");
    JwtSecretService svc(nullptr);
    Status st = svc.LoadOrInit();
    EXPECT_FALSE(st.ok());
    EXPECT_FALSE(svc.initialized());
}

// `AuthConfig_LoadDefaults_FirstStart`: fresh db → the §3.6 default key set is
// written, and the loaded snapshot matches the documented defaults.
TEST(AuthConfigServiceTest, LoadDefaultsFirstStart) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());

    AuthConfigService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInitDefaults().ok());

    EXPECT_EQ(RowCount(pdb.db(), "auth_config"), config::kAuthConfigDefaultKeyCount);

    config::AuthConfig c = svc.Get();
    EXPECT_EQ(c.access_token_ttl, 3600);
    EXPECT_EQ(c.refresh_token_ttl, 2592000);
    EXPECT_EQ(c.bcrypt_cost, 12);
    EXPECT_EQ(c.max_login_attempts, 5);
    EXPECT_EQ(c.lockout_duration, 900);
    EXPECT_EQ(c.password_min_length, 8);
    EXPECT_FALSE(c.email_verification);  // disabled by default
    EXPECT_TRUE(c.smtp_host.empty());    // null → empty
    EXPECT_EQ(c.smtp_port, 587);
    EXPECT_TRUE(c.smtp_tls);
}

// Idempotent: a second LoadOrInitDefaults does not duplicate rows (already
// populated → reload only).
TEST(AuthConfigServiceTest, LoadDefaultsIdempotent) {
    const std::string path = std::string(::testing::TempDir()) + "p08_cfg_idem.db";
    std::remove(path.c_str());

    {
        PlatformDb p1;
        ASSERT_TRUE(p1.Open(path).ok());
        AuthConfigService s1(p1.db());
        ASSERT_TRUE(s1.LoadOrInitDefaults().ok());
        EXPECT_EQ(RowCount(p1.db(), "auth_config"), config::kAuthConfigDefaultKeyCount);
    }
    {
        PlatformDb p2;
        ASSERT_TRUE(p2.Open(path).ok());
        AuthConfigService s2(p2.db());
        ASSERT_TRUE(s2.LoadOrInitDefaults().ok());
        EXPECT_EQ(RowCount(p2.db(), "auth_config"), config::kAuthConfigDefaultKeyCount);
    }
    std::remove(path.c_str());
}

// `AuthConfig_HotReload_OnChange`: an externally-modified row + ReloadKey →
// OnChange fires with the key AND the in-memory snapshot reflects the new value.
// (S7's admin API writes the row then calls ReloadKey; here we simulate the write.)
TEST(AuthConfigServiceTest, HotReloadOnChange) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    AuthConfigService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInitDefaults().ok());
    EXPECT_FALSE(svc.Get().email_verification);

    std::string fired_key;
    int fire_count = 0;
    svc.OnChange([&](const std::string& k) { fired_key = k; ++fire_count; });

    // Simulate the admin-API write (S7 does this), then hot-reload the key.
    ASSERT_EQ(sqlite3_exec(
                  pdb.db(),
                  "UPDATE auth_config SET value='true' WHERE key='email_verification'",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_TRUE(svc.ReloadKey("email_verification").ok());

    EXPECT_EQ(fire_count, 1);
    EXPECT_EQ(fired_key, "email_verification");
    EXPECT_TRUE(svc.Get().email_verification);  // snapshot updated
}

TEST(AuthConfigServiceTest, ReloadUnknownKeyReturnsNotFound) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    AuthConfigService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInitDefaults().ok());
    EXPECT_FALSE(svc.ReloadKey("no_such_key").ok());
}

}  // namespace
}  // namespace cortrix::auth
