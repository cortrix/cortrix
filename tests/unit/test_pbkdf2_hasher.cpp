#include <gtest/gtest.h>

#include <string>

#include "cortrix/auth/auth_service.h"
#include "cortrix/auth/pbkdf2_password_hasher.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/config/auth_config.h"

// P08 S2 (final): Pbkdf2PasswordHasher — the OpenSSL PBKDF2-HMAC-SHA256 production
// hasher (TECH_DEBT-AUTH-PBKDF2-PLACEHOLDER; a future compatibility update can add real
// bcrypt). Maps the §S2 hash test cases onto the placeholder (the `$2b$` format
// case becomes a `pbkdf2$` format assertion, documenting the deviation).
namespace cortrix::auth {
namespace {

// Low iterations so the suite stays fast (production default is 600k; the count
// is stored in the hash string so Verify is independent of this).
constexpr int kTestIters = 1000;

// `Password_HashAndVerify`: hash then verify the same password → true.
TEST(Pbkdf2HasherTest, HashAndVerify) {
    Pbkdf2PasswordHasher h(kTestIters);
    auto hash = h.Hash("Secure123!");
    ASSERT_TRUE(hash.ok()) << hash.status().message();
    auto ok = h.Verify("Secure123!", hash.value());
    ASSERT_TRUE(ok.ok());
    EXPECT_TRUE(ok.value());
}

// `Password_WrongPassword`: verify a different password → false (not an error).
TEST(Pbkdf2HasherTest, WrongPassword) {
    Pbkdf2PasswordHasher h(kTestIters);
    auto hash = h.Hash("Secure123!");
    ASSERT_TRUE(hash.ok());
    auto ok = h.Verify("WrongPass1", hash.value());
    ASSERT_TRUE(ok.ok());
    EXPECT_FALSE(ok.value());
}

// Placeholder format (replaces §S2 `Password_BcryptCost12`): the hash is the
// self-describing `pbkdf2$sha256$<iters>$<salt>$<dk>`, NOT `$2b$`. This test
// documents the intentional spec deviation (TECH_DEBT-AUTH-PBKDF2-PLACEHOLDER).
TEST(Pbkdf2HasherTest, PlaceholderFormatNotBcrypt) {
    Pbkdf2PasswordHasher h(kTestIters);
    auto hash = h.Hash("Secure123!");
    ASSERT_TRUE(hash.ok());
    const std::string& s = hash.value();
    EXPECT_EQ(s.rfind("pbkdf2$sha256$", 0), 0u) << s;
    EXPECT_EQ(s.find("$2b$"), std::string::npos)
        << "placeholder must NOT emit a bcrypt $2b$ hash";
    // iterations embedded in the string.
    EXPECT_NE(s.find("$" + std::to_string(kTestIters) + "$"), std::string::npos);
    // cost() still reports the bcrypt-equivalent factor 12 (P08 §4.2 intent).
    EXPECT_EQ(h.cost(), 12);
}

// Salt is random → two hashes of the same password differ, yet both verify.
TEST(Pbkdf2HasherTest, SaltedSoHashesDiffer) {
    Pbkdf2PasswordHasher h(kTestIters);
    auto h1 = h.Hash("Secure123!");
    auto h2 = h.Hash("Secure123!");
    ASSERT_TRUE(h1.ok() && h2.ok());
    EXPECT_NE(h1.value(), h2.value());  // different salt
    EXPECT_TRUE(h.Verify("Secure123!", h1.value()).value());
    EXPECT_TRUE(h.Verify("Secure123!", h2.value()).value());
}

// Verify is independent of the verifier's configured iteration count (it reads
// the count from the stored hash) — proves stored hashes survive a config change.
TEST(Pbkdf2HasherTest, VerifyUsesStoredIterations) {
    Pbkdf2PasswordHasher writer(kTestIters);
    auto hash = writer.Hash("Secure123!");
    ASSERT_TRUE(hash.ok());
    Pbkdf2PasswordHasher reader(99);  // different config
    auto ok = reader.Verify("Secure123!", hash.value());
    ASSERT_TRUE(ok.ok());
    EXPECT_TRUE(ok.value());
}

// Malformed stored hash → error (distinguishable from a wrong password).
TEST(Pbkdf2HasherTest, MalformedHashIsError) {
    Pbkdf2PasswordHasher h(kTestIters);
    EXPECT_FALSE(h.Verify("x", "not-a-pbkdf2-hash").ok());
    EXPECT_FALSE(h.Verify("x", "$2b$12$bcryptlookingbutwrong").ok());
    EXPECT_FALSE(h.Verify("x", "pbkdf2$sha256$abc$salt$dk").ok());  // bad iters
}

// End-to-end through AuthService: Register with the real hasher stores a
// pbkdf2$ hash (not plaintext), and Login verifies against it.
TEST(Pbkdf2HasherTest, EndToEndRegisterLoginWithRealHasher) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    Pbkdf2PasswordHasher hasher(kTestIters);
    config::AuthConfig cfg;
    AuthService svc(pdb.db(), cfg, &hasher,
                    "test-secret-key-at-least-32-bytes-long-for-testing");

    auto reg = svc.Register("user@example.com", "Secure123!", "User");
    ASSERT_TRUE(reg.ok()) << reg.status().message();

    auto login = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(login.ok()) << login.status().message();
    EXPECT_FALSE(login.value().access_token.empty());

    auto bad = svc.Login("user@example.com", "WrongPass1");
    EXPECT_FALSE(bad.ok());
}

}  // namespace
}  // namespace cortrix::auth
