#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/auth_service.h"
#include "cortrix/auth/password_utils.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/config/auth_config.h"

// P08 S2 coverage: ValidatePassword / ValidateEmail / GenerateUserId (§4.5) +
// Register flow (§4.1). The bcrypt library choice is abstracted behind
// IPasswordHasher; these tests use a fast fake so they are independent of it
// (the real bcrypt round-trip lives in test_bcrypt.cpp once the lib is wired).
namespace cortrix::auth {
namespace {

// A deterministic, fast stand-in for the bcrypt hasher: hash = "fake$" + pwd.
// Enough to exercise Register's flow (hash stored, no plaintext) without the
// real KDF cost.
class FakePasswordHasher : public IPasswordHasher {
public:
    Result<std::string> Hash(const std::string& password) override {
        return std::string("fake$") + password;
    }
    Result<bool> Verify(const std::string& password, const std::string& hash) override {
        return hash == (std::string("fake$") + password);
    }
    int cost() const override { return 12; }
};

// ----------------------------- Validation --------------------------------

TEST(PasswordValidationTest, PasswordTooShort) {
    EXPECT_FALSE(ValidatePassword("Abc1", 8));   // 4 chars
}
TEST(PasswordValidationTest, PasswordNoDigit) {
    EXPECT_FALSE(ValidatePassword("Abcdefgh", 8));
}
TEST(PasswordValidationTest, PasswordNoLetter) {
    EXPECT_FALSE(ValidatePassword("12345678", 8));
}
TEST(PasswordValidationTest, PasswordValid) {
    EXPECT_TRUE(ValidatePassword("Secure123", 8));
}
TEST(PasswordValidationTest, PasswordTooLongRejected) {
    EXPECT_FALSE(ValidatePassword(std::string(129, 'a') + "1", 8));  // 130 > 128
}
TEST(PasswordValidationTest, PasswordMinLengthConfigurable) {
    EXPECT_FALSE(ValidatePassword("Ab1", 8));
    EXPECT_TRUE(ValidatePassword("Ab1", 3));  // honors a lower min_length
}

TEST(EmailValidationTest, Valid) {
    EXPECT_TRUE(ValidateEmail("a@b.com"));
    EXPECT_TRUE(ValidateEmail("user.name+tag@sub.example.com"));
}
TEST(EmailValidationTest, Invalid) {
    EXPECT_FALSE(ValidateEmail("not-an-email"));
    EXPECT_FALSE(ValidateEmail("a@b"));            // no dot in domain
    EXPECT_FALSE(ValidateEmail("a@@b.com"));       // two '@'
    EXPECT_FALSE(ValidateEmail("@b.com"));         // empty local
    EXPECT_FALSE(ValidateEmail("a@.com"));         // domain starts with dot
    EXPECT_FALSE(ValidateEmail("a b@c.com"));      // whitespace
    EXPECT_FALSE(ValidateEmail(""));
}

TEST(GenerateUserIdTest, FormatAndUniqueness) {
    const std::string id = GenerateUserId();
    EXPECT_EQ(id.rfind("usr_", 0), 0u);
    EXPECT_EQ(id.size(), 12u);  // "usr_" + 8 hex
    for (char c : id.substr(4)) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
    }
    // Two calls should differ (random) — not a guarantee but overwhelmingly likely.
    EXPECT_NE(GenerateUserId(), GenerateUserId());
}

// ------------------------------- Register ---------------------------------

class RegisterTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(pdb_.Open(":memory:").ok()); }
    AuthService MakeService() {
        return AuthService(pdb_.db(), config::AuthConfig{}, &hasher_,
                           "test-secret-key-at-least-32-bytes-long-for-testing");
    }
    int UserRowCount() {
        sqlite3_stmt* s = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(pdb_.db(), "SELECT COUNT(*) FROM users", -1, &s,
                                     nullptr),
                  SQLITE_OK);
        int n = -1;
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        return n;
    }
    PlatformDb pdb_;
    FakePasswordHasher hasher_;
};

TEST_F(RegisterTest, Success) {
    AuthService svc = MakeService();
    auto r = svc.Register("user@example.com", "Secure123!", "Zhang San");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().email, "user@example.com");
    EXPECT_EQ(r.value().display_name, "Zhang San");
    EXPECT_FALSE(r.value().email_verified);
    EXPECT_EQ(r.value().id.rfind("usr_", 0), 0u);
    EXPECT_GT(r.value().created_at, 0);
    EXPECT_TRUE(r.value().tenants.empty());  // P09 wiring is D3.5
    EXPECT_EQ(UserRowCount(), 1);

    // Stored hash is the hasher output, NOT the plaintext.
    sqlite3_stmt* s = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(pdb_.db(),
                                 "SELECT password_hash FROM users WHERE email=?", -1,
                                 &s, nullptr),
              SQLITE_OK);
    sqlite3_bind_text(s, 1, "user@example.com", -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    std::string stored = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    EXPECT_EQ(stored, "fake$Secure123!");
    EXPECT_EQ(stored.find("Secure123!"), 5u);  // not stored as bare plaintext
}

TEST_F(RegisterTest, DuplicateEmail) {
    AuthService svc = MakeService();
    ASSERT_TRUE(svc.Register("dup@example.com", "Secure123!", "First").ok());

    auto r2 = svc.Register("dup@example.com", "Secure123!", "Second");
    ASSERT_FALSE(r2.ok());
    EXPECT_EQ(r2.status().code(), StatusCode::kAlreadyExists);
    EXPECT_NE(r2.status().message().find("CX_ERR_AUTH_EMAIL_ALREADY_EXISTS"),
              std::string::npos);
    EXPECT_EQ(UserRowCount(), 1);  // no second row
}

TEST_F(RegisterTest, InvalidPassword) {
    AuthService svc = MakeService();
    auto r = svc.Register("user@example.com", "weak", "Name");  // too short, no digit
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(r.status().message().find("CX_ERR_INVALID_REQUEST"), std::string::npos);
    EXPECT_EQ(UserRowCount(), 0);
}

TEST_F(RegisterTest, InvalidEmailRejected) {
    AuthService svc = MakeService();
    auto r = svc.Register("not-an-email", "Secure123!", "Name");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(UserRowCount(), 0);
}

TEST_F(RegisterTest, EmptyDisplayNameRejected) {
    AuthService svc = MakeService();
    auto r = svc.Register("user@example.com", "Secure123!", "");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(UserRowCount(), 0);
}

TEST_F(RegisterTest, GetUserInfoRoundTrip) {
    AuthService svc = MakeService();
    auto r = svc.Register("me@example.com", "Secure123!", "Me");
    ASSERT_TRUE(r.ok());

    auto got = svc.GetUserInfo(r.value().id);
    ASSERT_TRUE(got.ok()) << got.status().message();
    EXPECT_EQ(got.value().email, "me@example.com");
    EXPECT_EQ(got.value().display_name, "Me");

    auto missing = svc.GetUserInfo("usr_deadbeef");
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), StatusCode::kNotFound);
    EXPECT_NE(missing.status().message().find("CX_ERR_USER_NOT_FOUND"),
              std::string::npos);
}

}  // namespace
}  // namespace cortrix::auth
