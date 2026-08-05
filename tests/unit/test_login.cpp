#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/auth_service.h"
#include "cortrix/auth/jwt_utils.h"
#include "cortrix/auth/password_utils.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/config/auth_config.h"

// Auth S3 coverage: Login / Logout / RefreshAccessToken / ValidateAccessToken
// (§2.3-2.5 / §4.2-4.4 / §4.7) — account lockout, token blacklist, refresh
// revoke. Uses the fake hasher (bcrypt-independent; real bcrypt round-trip is in
// test_bcrypt.cpp once the lib lands).
namespace cortrix::auth {
namespace {

const std::string kSecret = "test-secret-key-at-least-32-bytes-long-for-testing";

class FakePasswordHasher : public IPasswordHasher {
public:
    Result<std::string> Hash(const std::string& p) override { return std::string("fake$") + p; }
    Result<bool> Verify(const std::string& p, const std::string& h) override {
        return h == (std::string("fake$") + p);
    }
    int cost() const override { return 12; }
};

class LoginTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        config_.max_login_attempts = 5;
        config_.lockout_duration = 900;
    }
    AuthService Svc() { return AuthService(pdb_.db(), config_, &hasher_, kSecret); }

    // Register a baseline user via the service (so the stored hash is consistent).
    std::string Seed(const std::string& email = "user@example.com",
                     const std::string& pwd = "Secure123!") {
        auto r = Svc().Register(email, pwd, "User");
        EXPECT_TRUE(r.ok()) << r.status().message();
        return r.ok() ? r.value().id : "";
    }
    void Exec(const std::string& sql) {
        ASSERT_EQ(sqlite3_exec(pdb_.db(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK)
            << sql;
    }
    int RefreshRowCount(const char* where = "1") {
        sqlite3_stmt* s = nullptr;
        std::string q = std::string("SELECT COUNT(*) FROM refresh_tokens WHERE ") + where;
        EXPECT_EQ(sqlite3_prepare_v2(pdb_.db(), q.c_str(), -1, &s, nullptr), SQLITE_OK);
        int n = -1;
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        return n;
    }

    PlatformDb pdb_;
    config::AuthConfig config_;
    FakePasswordHasher hasher_;
};

// ------------------------------- Login ------------------------------------

TEST_F(LoginTest, Success) {
    Seed();
    auto r = Svc().Login("user@example.com", "Secure123!");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_FALSE(r.value().access_token.empty());
    EXPECT_FALSE(r.value().refresh_token.empty());
    EXPECT_EQ(r.value().expires_in, config_.access_token_ttl);

    // access token decodes + carries this user.
    auto dec = JwtCodec::Decode(r.value().access_token, {kSecret}, /*now*/ 0 + 1);
    // (now=1 is < exp, which is ~real-now+3600, so not expired)
    ASSERT_TRUE(dec.ok()) << dec.status().message();
    EXPECT_EQ(dec.value().email, "user@example.com");
    EXPECT_FALSE(dec.value().is_refresh());

    // a refresh_tokens row was persisted.
    EXPECT_EQ(RefreshRowCount(), 1);
}

TEST_F(LoginTest, WrongPassword) {
    Seed();
    auto r = Svc().Login("user@example.com", "WrongPass1");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kUnauthenticated);
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_INVALID_CREDENTIALS"), std::string::npos);
}

TEST_F(LoginTest, NonexistentEmail) {
    auto r = Svc().Login("ghost@example.com", "Secure123!");
    ASSERT_FALSE(r.ok());
    // anti-enumeration: same code as wrong password.
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_INVALID_CREDENTIALS"), std::string::npos);
}

TEST_F(LoginTest, AccountLockedAfterMaxAttempts) {
    Seed();
    AuthService svc = Svc();
    for (int i = 0; i < config_.max_login_attempts; ++i) {
        auto r = svc.Login("user@example.com", "WrongPass1");
        EXPECT_FALSE(r.ok());
    }
    // Now locked: even the correct password is refused with ACCOUNT_LOCKED.
    auto locked = svc.Login("user@example.com", "Secure123!");
    ASSERT_FALSE(locked.ok());
    EXPECT_EQ(locked.status().code(), StatusCode::kPermissionDenied);
    EXPECT_NE(locked.status().message().find("CX_ERR_ACCOUNT_LOCKED"), std::string::npos);
    EXPECT_NE(locked.status().message().find("remaining_ms="), std::string::npos);
}

TEST_F(LoginTest, AutoUnlockAfterLockoutExpires) {
    Seed();
    AuthService svc = Svc();
    for (int i = 0; i < config_.max_login_attempts; ++i) {
        svc.Login("user@example.com", "WrongPass1");
    }
    // Force the lock to have expired (locked_until in the past).
    Exec("UPDATE users SET locked_until=1 WHERE email='user@example.com'");
    auto r = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(r.ok()) << r.status().message();  // auto-unlocked + logged in
}

TEST_F(LoginTest, DisabledAccountRejected) {
    Seed();
    Exec("UPDATE users SET status='disabled' WHERE email='user@example.com'");
    auto r = Svc().Login("user@example.com", "Secure123!");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_ACCOUNT_DISABLED"), std::string::npos);
}

// ------------------------------ Logout ------------------------------------

TEST_F(LoginTest, LogoutBlacklistsAccessToken) {
    Seed();
    AuthService svc = Svc();
    auto login = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(login.ok());

    // Before logout: access token validates.
    ASSERT_TRUE(svc.ValidateAccessToken(login.value().access_token).ok());

    ASSERT_TRUE(svc.Logout(login.value().access_token).ok());

    // After logout: same token is blacklisted → CX_ERR_AUTH_TOKEN_REVOKED.
    auto v = svc.ValidateAccessToken(login.value().access_token);
    ASSERT_FALSE(v.ok());
    EXPECT_NE(v.status().message().find("CX_ERR_AUTH_TOKEN_REVOKED"), std::string::npos);
}

TEST_F(LoginTest, LogoutRevokesRefreshToken) {
    Seed();
    AuthService svc = Svc();
    auto login = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(login.ok());

    ASSERT_TRUE(svc.Logout(login.value().access_token).ok());

    // The refresh token is now revoked → refresh fails with TOKEN_REVOKED.
    auto r = svc.RefreshAccessToken(login.value().refresh_token);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_TOKEN_REVOKED"), std::string::npos);
    EXPECT_EQ(RefreshRowCount("revoked=1"), 1);
}

TEST_F(LoginTest, LogoutInvalidTokenIsNoOpSuccess) {
    auto svc = Svc();
    EXPECT_TRUE(svc.Logout("garbage.token.value").ok());  // idempotent
}

// ------------------------------ Refresh -----------------------------------

TEST_F(LoginTest, RefreshSuccess) {
    Seed();
    AuthService svc = Svc();
    auto login = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(login.ok());

    auto r = svc.RefreshAccessToken(login.value().refresh_token);
    ASSERT_TRUE(r.ok()) << r.status().message();
    // new access token validates + is an access token for this user.
    auto v = svc.ValidateAccessToken(r.value());
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value().email, "user@example.com");
}

TEST_F(LoginTest, RefreshRevokedToken) {
    Seed();
    AuthService svc = Svc();
    auto login = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(login.ok());
    Exec("UPDATE refresh_tokens SET revoked=1");
    auto r = svc.RefreshAccessToken(login.value().refresh_token);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_TOKEN_REVOKED"), std::string::npos);
}

TEST_F(LoginTest, RefreshExpiredTokenRow) {
    Seed();
    AuthService svc = Svc();
    auto login = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(login.ok());
    // Expire the stored row (JWT itself is long-lived; the row gate fires first).
    Exec("UPDATE refresh_tokens SET expires_at=1");
    auto r = svc.RefreshAccessToken(login.value().refresh_token);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_TOKEN_EXPIRED"), std::string::npos);
}

TEST_F(LoginTest, RefreshWithAccessTokenRejected) {
    Seed();
    AuthService svc = Svc();
    auto login = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(login.ok());
    // Passing the ACCESS token where a refresh token is expected → invalid refresh.
    auto r = svc.RefreshAccessToken(login.value().access_token);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_INVALID_REFRESH_TOKEN"),
              std::string::npos);
}

TEST_F(LoginTest, RefreshGarbageTokenRejected) {
    auto svc = Svc();
    auto r = svc.RefreshAccessToken("not.a.jwt");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_INVALID_REFRESH_TOKEN"),
              std::string::npos);
}

// --------------------------- Validate + blacklist load --------------------

TEST_F(LoginTest, ValidateInvalidAndExpired) {
    auto svc = Svc();
    EXPECT_FALSE(svc.ValidateAccessToken("garbage").ok());

    // A token signed by a different secret → unauthorized.
    JwtPayload p;
    p.sub = "usr_x"; p.sid = "sess_x"; p.iat = 1; p.exp = 9999999999;
    auto foreign = JwtCodec::Encode(p, "some-other-secret-key-32-bytes-xxxxxx");
    auto v = svc.ValidateAccessToken(foreign.value());
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(v.status().code(), StatusCode::kUnauthenticated);
}

TEST_F(LoginTest, BlacklistSurvivesReloadFromDb) {
    const std::string path = std::string(::testing::TempDir()) + "p08_blacklist.db";
    std::remove(path.c_str());

    std::string access;
    {
        PlatformDb p1;
        ASSERT_TRUE(p1.Open(path).ok());
        AuthService s1(p1.db(), config_, &hasher_, kSecret);
        ASSERT_TRUE(s1.Register("u@e.com", "Secure123!", "U").ok());
        auto login = s1.Login("u@e.com", "Secure123!");
        ASSERT_TRUE(login.ok());
        access = login.value().access_token;
        ASSERT_TRUE(s1.Logout(access).ok());  // persists to token_blacklist
    }
    {
        PlatformDb p2;
        ASSERT_TRUE(p2.Open(path).ok());
        AuthService s2(p2.db(), config_, &hasher_, kSecret);
        ASSERT_TRUE(s2.LoadBlacklist().ok());  // load persisted blacklist
        auto v = s2.ValidateAccessToken(access);
        ASSERT_FALSE(v.ok());  // still revoked across restart
        EXPECT_NE(v.status().message().find("CX_ERR_AUTH_TOKEN_REVOKED"), std::string::npos);
    }
    std::remove(path.c_str());
}

}  // namespace
}  // namespace cortrix::auth
