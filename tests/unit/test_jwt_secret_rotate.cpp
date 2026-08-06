#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "cortrix/auth/jwt_secret_service.h"
#include "cortrix/auth/jwt_utils.h"
#include "cortrix/auth/platform_db.h"

// Auth S7: JWT secret rotation + dual-key window + 24h prev cron (/
//, Issue 1.1 C).
namespace cortrix::auth {
namespace {

JwtPayload MakeToken(const std::string& sub) {
    JwtPayload p;
    p.sub = sub;
    p.sid = "sess_1";
    p.iat = 1;
    p.exp = 4102444800;  // far future (year 2100) so expiry never trips here
    return p;
}

int SecretRowCount(sqlite3* db, const char* status) {
    sqlite3_stmt* s = nullptr;
    std::string q = std::string("SELECT COUNT(*) FROM auth_secrets WHERE status='") + status + "'";
    EXPECT_EQ(sqlite3_prepare_v2(db, q.c_str(), -1, &s, nullptr), SQLITE_OK);
    int n = -1;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

// `JwtSecret_Rotate_ViaAdminApi`: rotate → new current, old → prev, window opens.
TEST(JwtRotateTest, RotateMintsNewCurrentAndDemotesPrev) {
    ::unsetenv("CORTRIX_JWT_SECRET");
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    JwtSecretService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInit().ok());
    const std::string old_secret = svc.current_secret();

    auto r = svc.RotateJwtSecret();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_FALSE(r.value().new_key_id.empty());
    EXPECT_FALSE(r.value().prev_key_id.empty());
    EXPECT_GT(r.value().prev_expires_at_ms, 0);

    // current changed; exactly one current + one prev now.
    EXPECT_NE(svc.current_secret(), old_secret);
    EXPECT_EQ(SecretRowCount(pdb.db(), "current"), 1);
    EXPECT_EQ(SecretRowCount(pdb.db(), "prev"), 1);
}

// `JwtSecret_DoubleVerify_PrevValid`: a token signed with the OLD secret still
// verifies during the 24h window (accept set = current + valid prev).
TEST(JwtRotateTest, DoubleVerifyPrevValid) {
    ::unsetenv("CORTRIX_JWT_SECRET");
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    JwtSecretService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInit().ok());
    const std::string old_secret = svc.current_secret();

    // Token signed with old secret BEFORE rotation.
    auto tok = JwtCodec::Encode(MakeToken("usr_a"), old_secret);
    ASSERT_TRUE(tok.ok());

    ASSERT_TRUE(svc.RotateJwtSecret().ok());

    // accept set contains current + prev → old-signed token still verifies.
    auto accept = svc.GetAcceptSecrets();
    ASSERT_TRUE(accept.ok());
    ASSERT_EQ(accept.value().size(), 2u);
    auto dec = JwtCodec::Decode(tok.value(), accept.value(), /*now*/ 2);
    ASSERT_TRUE(dec.ok()) << dec.status().message();
    EXPECT_EQ(dec.value().sub, "usr_a");

    // A token signed with the NEW current also verifies.
    auto tok2 = JwtCodec::Encode(MakeToken("usr_b"), svc.current_secret());
    ASSERT_TRUE(JwtCodec::Decode(tok2.value(), accept.value(), 2).ok());
}

// `JwtSecret_PrevExpired_AfterCron`: once the prev window passes + cron retires
// it, an old-signed token no longer verifies.
TEST(JwtRotateTest, PrevExpiredAfterCron) {
    ::unsetenv("CORTRIX_JWT_SECRET");
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    JwtSecretService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInit().ok());
    const std::string old_secret = svc.current_secret();
    auto tok = JwtCodec::Encode(MakeToken("usr_a"), old_secret);
    ASSERT_TRUE(svc.RotateJwtSecret().ok());

    // Force the prev to have expired, then run the cron.
    ASSERT_EQ(sqlite3_exec(pdb.db(),
                           "UPDATE auth_secrets SET expires_at=1 WHERE status='prev'",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    auto cleaned = svc.CleanupExpiredPrev();
    ASSERT_TRUE(cleaned.ok());
    EXPECT_EQ(cleaned.value(), 1);  // one prev retired
    EXPECT_EQ(SecretRowCount(pdb.db(), "prev"), 0);

    // accept set is now current-only → the old-signed token is rejected.
    auto accept = svc.GetAcceptSecrets();
    ASSERT_TRUE(accept.ok());
    EXPECT_EQ(accept.value().size(), 1u);
    auto dec = JwtCodec::Decode(tok.value(), accept.value(), 2);
    EXPECT_FALSE(dec.ok());
}

// GetAcceptSecrets also skips a prev that is expired but NOT yet retired by cron
// (defensive — the window is enforced on read, not only by the cron).
TEST(JwtRotateTest, AcceptSecretsSkipsExpiredPrevBeforeCron) {
    ::unsetenv("CORTRIX_JWT_SECRET");
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    JwtSecretService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInit().ok());
    ASSERT_TRUE(svc.RotateJwtSecret().ok());
    ASSERT_EQ(sqlite3_exec(pdb.db(),
                           "UPDATE auth_secrets SET expires_at=1 WHERE status='prev'",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    // prev row still present (cron not run) but expired → not in accept set.
    EXPECT_EQ(SecretRowCount(pdb.db(), "prev"), 1);
    auto accept = svc.GetAcceptSecrets();
    ASSERT_TRUE(accept.ok());
    EXPECT_EQ(accept.value().size(), 1u);  // current only
}

// `JwtSecret_Persist_AcrossRestart` after a rotation: the rotated current loads.
TEST(JwtRotateTest, RotatedCurrentPersistsAcrossRestart) {
    ::unsetenv("CORTRIX_JWT_SECRET");
    const std::string path = std::string(::testing::TempDir()) + "auth_rotate_restart.db";
    std::remove(path.c_str());

    std::string rotated;
    {
        PlatformDb p1;
        ASSERT_TRUE(p1.Open(path).ok());
        JwtSecretService s1(p1.db());
        ASSERT_TRUE(s1.LoadOrInit().ok());
        ASSERT_TRUE(s1.RotateJwtSecret().ok());
        rotated = s1.current_secret();
    }
    {
        PlatformDb p2;
        ASSERT_TRUE(p2.Open(path).ok());
        JwtSecretService s2(p2.db());
        ASSERT_TRUE(s2.LoadOrInit().ok());
        EXPECT_EQ(s2.current_secret(), rotated);  // loads the rotated current
    }
    std::remove(path.c_str());
}

}  // namespace
}  // namespace cortrix::auth
