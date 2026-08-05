#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/auth_service.h"
#include "cortrix/auth/email_sender.h"
#include "cortrix/auth/password_utils.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/config/auth_config.h"

// Auth S5: password reset + email verification (§2.6-2.8 / §4.5 / §4.6). Uses the
// fake hasher + a capturing NullEmailSender so the verification code is readable
// without real delivery (bcrypt-independent; SMTP delivery is D3.5).
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

class ResetTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(pdb_.Open(":memory:").ok()); }
    AuthService Svc() {
        return AuthService(pdb_.db(), config::AuthConfig{}, &hasher_, kSecret, &mail_);
    }
    void Seed(const std::string& email = "user@example.com",
              const std::string& pwd = "Secure123!") {
        ASSERT_TRUE(Svc().Register(email, pwd, "User").ok());
    }
    // Read the latest code for (email,type) directly from the table (tests act as
    // the "email client" receiving the code).
    std::string LatestCode(const std::string& email, const std::string& type) {
        sqlite3_stmt* s = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(
                      pdb_.db(),
                      "SELECT code FROM verification_codes WHERE email=? AND type=? "
                      "ORDER BY created_at DESC, id DESC LIMIT 1",
                      -1, &s, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(s, 1, email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, type.c_str(), -1, SQLITE_TRANSIENT);
        std::string code;
        if (sqlite3_step(s) == SQLITE_ROW)
            code = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
        return code;
    }
    void Exec(const std::string& sql) {
        ASSERT_EQ(sqlite3_exec(pdb_.db(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }
    int CodeRowCount() {
        sqlite3_stmt* s = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(pdb_.db(), "SELECT COUNT(*) FROM verification_codes",
                                     -1, &s, nullptr), SQLITE_OK);
        int n = -1;
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        return n;
    }

    PlatformDb pdb_;
    FakePasswordHasher hasher_;
    NullEmailSender mail_;
};

// ---- GenerateVerificationCode ----

TEST(VerificationCodeTest, SixDigitsNumeric) {
    for (int i = 0; i < 50; ++i) {
        const std::string c = AuthService::GenerateVerificationCode();
        ASSERT_EQ(c.size(), 6u) << c;
        for (char ch : c) EXPECT_TRUE(ch >= '0' && ch <= '9') << c;
    }
}

// ---- Request ----

// `ResetRequest_ExistingEmail`: existing email → 200 + a password_reset code row.
TEST_F(ResetTest, RequestExistingEmail) {
    Seed();
    AuthService svc = Svc();
    ASSERT_TRUE(svc.RequestPasswordReset("user@example.com").ok());
    EXPECT_FALSE(LatestCode("user@example.com", "password_reset").empty());
    EXPECT_TRUE(mail_.last().sent);  // routed to the (Null) sender
    EXPECT_EQ(mail_.last().to, "user@example.com");
}

// `ResetRequest_NonexistentEmail`: unknown email → still 200 (anti-enumeration), no code row.
TEST_F(ResetTest, RequestNonexistentEmail) {
    AuthService svc = Svc();
    ASSERT_TRUE(svc.RequestPasswordReset("ghost@example.com").ok());  // same OK response
    EXPECT_EQ(CodeRowCount(), 0);  // no code issued
}

// ---- Confirm ----

// `ResetConfirm_ValidCode`: correct code + valid new pwd → 200, password updated.
TEST_F(ResetTest, ConfirmValidCode) {
    Seed();
    AuthService svc = Svc();
    ASSERT_TRUE(svc.RequestPasswordReset("user@example.com").ok());
    const std::string code = LatestCode("user@example.com", "password_reset");

    ASSERT_TRUE(svc.ConfirmPasswordReset("user@example.com", code, "NewSecure456").ok());

    // Old password no longer works; new one does.
    EXPECT_FALSE(svc.Login("user@example.com", "Secure123!").ok());
    EXPECT_TRUE(svc.Login("user@example.com", "NewSecure456").ok());
}

// `ResetConfirm_ExpiredCode`.
TEST_F(ResetTest, ConfirmExpiredCode) {
    Seed();
    AuthService svc = Svc();
    ASSERT_TRUE(svc.RequestPasswordReset("user@example.com").ok());
    const std::string code = LatestCode("user@example.com", "password_reset");
    Exec("UPDATE verification_codes SET expires_at=1 WHERE type='password_reset'");
    auto st = svc.ConfirmPasswordReset("user@example.com", code, "NewSecure456");
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_AUTH_INVALID_RESET_CODE"), std::string::npos);
}

// `ResetConfirm_WrongCode`.
TEST_F(ResetTest, ConfirmWrongCode) {
    Seed();
    AuthService svc = Svc();
    ASSERT_TRUE(svc.RequestPasswordReset("user@example.com").ok());
    auto st = svc.ConfirmPasswordReset("user@example.com", "000000", "NewSecure456");
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_AUTH_INVALID_RESET_CODE"), std::string::npos);
}

// `ResetConfirm_UsedCode`: a code can't be reused.
TEST_F(ResetTest, ConfirmUsedCode) {
    Seed();
    AuthService svc = Svc();
    ASSERT_TRUE(svc.RequestPasswordReset("user@example.com").ok());
    const std::string code = LatestCode("user@example.com", "password_reset");
    ASSERT_TRUE(svc.ConfirmPasswordReset("user@example.com", code, "NewSecure456").ok());
    // second use → invalid.
    auto st = svc.ConfirmPasswordReset("user@example.com", code, "AnotherPass7");
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_AUTH_INVALID_RESET_CODE"), std::string::npos);
}

// `ResetConfirm_InvalidNewPassword`.
TEST_F(ResetTest, ConfirmInvalidNewPassword) {
    Seed();
    AuthService svc = Svc();
    ASSERT_TRUE(svc.RequestPasswordReset("user@example.com").ok());
    const std::string code = LatestCode("user@example.com", "password_reset");
    auto st = svc.ConfirmPasswordReset("user@example.com", code, "weak");  // no digit/short
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_INVALID_REQUEST"), std::string::npos);
}

// `ResetConfirm_RevokesRefreshTokens`: after reset, all the user's refresh tokens
// are revoked (§2.7 side-effect).
TEST_F(ResetTest, ConfirmRevokesRefreshTokens) {
    Seed();
    AuthService svc = Svc();
    auto login = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(login.ok());

    ASSERT_TRUE(svc.RequestPasswordReset("user@example.com").ok());
    const std::string code = LatestCode("user@example.com", "password_reset");
    ASSERT_TRUE(svc.ConfirmPasswordReset("user@example.com", code, "NewSecure456").ok());

    // The pre-reset refresh token is now revoked.
    auto r = svc.RefreshAccessToken(login.value().refresh_token);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_TOKEN_REVOKED"), std::string::npos);
}

// ---- Verify email ----

// `VerifyEmail_ValidCode`.
TEST_F(ResetTest, VerifyEmailValidCode) {
    Seed();
    AuthService svc = Svc();
    ASSERT_TRUE(svc.IssueVerificationCode("user@example.com", "email_verify").ok());
    const std::string code = LatestCode("user@example.com", "email_verify");
    ASSERT_TRUE(svc.VerifyEmail("user@example.com", code).ok());

    // Read the email_verified flag directly.
    sqlite3_stmt* s = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(pdb_.db(), "SELECT email_verified FROM users WHERE email=?",
                                 -1, &s, nullptr), SQLITE_OK);
    sqlite3_bind_text(s, 1, "user@example.com", -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(s, 0), 1);
    sqlite3_finalize(s);
}

// `VerifyEmail_ExpiredCode`.
TEST_F(ResetTest, VerifyEmailExpiredCode) {
    Seed();
    AuthService svc = Svc();
    ASSERT_TRUE(svc.IssueVerificationCode("user@example.com", "email_verify").ok());
    const std::string code = LatestCode("user@example.com", "email_verify");
    Exec("UPDATE verification_codes SET expires_at=1 WHERE type='email_verify'");
    auto st = svc.VerifyEmail("user@example.com", code);
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_AUTH_INVALID_RESET_CODE"), std::string::npos);
}

// `VerifyEmail_AlreadyVerified`: idempotent — succeeds even with a bogus code once
// the user is already verified (§2.8).
TEST_F(ResetTest, VerifyEmailAlreadyVerified) {
    Seed();
    AuthService svc = Svc();
    ASSERT_TRUE(svc.IssueVerificationCode("user@example.com", "email_verify").ok());
    const std::string code = LatestCode("user@example.com", "email_verify");
    ASSERT_TRUE(svc.VerifyEmail("user@example.com", code).ok());
    // Already verified → a second call (even with a non-code) is a no-op success.
    EXPECT_TRUE(svc.VerifyEmail("user@example.com", "000000").ok());
}

// `EmailSender_LogMode`: NullEmailSender does not "deliver" but records the
// message (so the code is recoverable) and always succeeds.
TEST(EmailSenderTest, NullSenderLogsAndSucceeds) {
    NullEmailSender s;
    ASSERT_TRUE(s.Send("a@b.com", "subj", "body with code 123456").ok());
    EXPECT_TRUE(s.last().sent);
    EXPECT_EQ(s.last().to, "a@b.com");
    EXPECT_NE(s.last().body.find("123456"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::auth
