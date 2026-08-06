#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdio>
#include <string>

#include "cortrix/auth/auth_config_service.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/config/auth_config.h"

// Auth S7: SMTP admin API (Issue 8) — SetSmtp persists + hot-reloads;
// GetSmtpRedacted masks the password.
namespace cortrix::auth {
namespace {

std::string ReadKey(sqlite3* db, const std::string& key) {
    sqlite3_stmt* s = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, "SELECT value FROM auth_config WHERE key=?", -1, &s,
                                 nullptr), SQLITE_OK);
    sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string v;
    if (sqlite3_step(s) == SQLITE_ROW) v = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    return v;
}

// `Smtp_Configure_ViaAdminApi`: SetSmtp persists all keys + flips email_verification.
TEST(SmtpAdminTest, ConfigurePersistsAndEnables) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    AuthConfigService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInitDefaults().ok());
    EXPECT_FALSE(svc.Get().email_verification);  // default off

    AuthConfigService::SmtpSettings s;
    s.host = "smtp.example.com";
    s.port = 2525;
    s.user = "noreply@cortrix.ai";
    s.pass = "super-secret-pw";
    s.tls = true;
    s.enable_email_verification = true;
    ASSERT_TRUE(svc.SetSmtp(s).ok());

    // In-memory snapshot updated (hot reload).
    config::AuthConfig c = svc.Get();
    EXPECT_EQ(c.smtp_host, "smtp.example.com");
    EXPECT_EQ(c.smtp_port, 2525);
    EXPECT_EQ(c.smtp_user, "noreply@cortrix.ai");
    EXPECT_EQ(c.smtp_pass, "super-secret-pw");
    EXPECT_TRUE(c.smtp_tls);
    EXPECT_TRUE(c.email_verification);

    // Persisted to db (JSON-encoded).
    EXPECT_EQ(ReadKey(pdb.db(), "smtp.host"), "\"smtp.example.com\"");
    EXPECT_EQ(ReadKey(pdb.db(), "smtp.port"), "2525");
    EXPECT_EQ(ReadKey(pdb.db(), "email_verification"), "true");
}

// GET endpoint masks the password (never return the stored pass).
TEST(SmtpAdminTest, GetRedactedMasksPassword) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    AuthConfigService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInitDefaults().ok());

    // Before config: pass empty → redacted is empty (not "***").
    EXPECT_EQ(svc.GetSmtpRedacted().pass, "");

    AuthConfigService::SmtpSettings s;
    s.host = "smtp.x.com";
    s.pass = "secret";
    ASSERT_TRUE(svc.SetSmtp(s).ok());

    auto red = svc.GetSmtpRedacted();
    EXPECT_EQ(red.host, "smtp.x.com");
    EXPECT_EQ(red.pass, "***");           // masked
    EXPECT_NE(red.pass, "secret");        // never the real value
}

// SetSmtp fires OnChange for the affected keys (Issue 8 — live SmtpEmailSender
// re-reads without a restart).
TEST(SmtpAdminTest, SetSmtpFiresOnChange) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    AuthConfigService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInitDefaults().ok());

    int host_changes = 0, verify_changes = 0;
    svc.OnChange([&](const std::string& k) {
        if (k == "smtp.host") ++host_changes;
        if (k == "email_verification") ++verify_changes;
    });

    AuthConfigService::SmtpSettings s;
    s.host = "smtp.y.com";
    s.enable_email_verification = true;
    ASSERT_TRUE(svc.SetSmtp(s).ok());

    EXPECT_EQ(host_changes, 1);
    EXPECT_EQ(verify_changes, 1);
}

// Re-loading from a fresh service over the same db reflects the SMTP config
// (persistence across restart).
TEST(SmtpAdminTest, ConfigPersistsAcrossRestart) {
    const std::string path = std::string(::testing::TempDir()) + "auth_smtp_restart.db";
    std::remove(path.c_str());
    {
        PlatformDb p1;
        ASSERT_TRUE(p1.Open(path).ok());
        AuthConfigService s1(p1.db());
        ASSERT_TRUE(s1.LoadOrInitDefaults().ok());
        AuthConfigService::SmtpSettings s;
        s.host = "smtp.persist.com";
        s.enable_email_verification = true;
        ASSERT_TRUE(s1.SetSmtp(s).ok());
    }
    {
        PlatformDb p2;
        ASSERT_TRUE(p2.Open(path).ok());
        AuthConfigService s2(p2.db());
        ASSERT_TRUE(s2.LoadOrInitDefaults().ok());  // loads existing rows
        EXPECT_EQ(s2.Get().smtp_host, "smtp.persist.com");
        EXPECT_TRUE(s2.Get().email_verification);
    }
    std::remove(path.c_str());
}

}  // namespace
}  // namespace cortrix::auth
