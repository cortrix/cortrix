/// @file test_sec_jwt_hardening.cpp
/// @brief Security matrix B (JWT hardening) + matrix C (API-key + bootstrap token).
///
/// MATRIX B — attacks against the HS256 JWT path. Exercised at two layers:
///   1. JwtCodec::Decode (crypto core, src/auth/jwt_utils.cpp): forge attack tokens
///      directly (alg=none, algorithm-confusion, tampered payload, wrong signature,
///      expired) and assert each is rejected.
///   2. AuthService::ValidateAccessToken (the middleware decision the route
///      WithAuth wrapper calls): real register/login mints a valid token (positive
///      control) and rotation invalidates an old-secret token.
///   Every attack MUST be rejected (kUnauthenticated / kTokenExpired). A token that
///   sails through is a REAL VULNERABILITY; the test asserts the SECURE outcome and
///   thus fails loudly if the weakness exists.
///
/// MATRIX C — API-key + bootstrap token:
///   - ApiKeyService::ValidateApiKey: not_found / revoked / expired -> CX_ERR_AUTH_INVALID_API_KEY.
///   - BootstrapHandler::Consume: reuse / after-consumed -> CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID.
///
/// NBF note (read jwt_utils.h / jwt_utils.cpp): the Cortrix JwtPayload models no
/// `nbf` claim and Decode does not enforce not-before — Cortrix never mints nbf, so
/// a forged nbf-in-future claim is simply an extra JSON field that is ignored. We
/// document this as a DEFERRED case (not a forgeable bypass: such a token still
/// needs a valid HS256 signature, which an attacker cannot produce) rather than
/// faking coverage of an unenforced field.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/auth_service.h"
#include "cortrix/auth/bootstrap_handler.h"
#include "cortrix/auth/jwt_utils.h"
#include "cortrix/auth/password_utils.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/common/status.h"
#include "cortrix/config/auth_config.h"

namespace cortrix::auth {
namespace {

using json = nlohmann::json;

const std::string kSecret = "sec-test-secret-key-at-least-32-bytes-long-aaaa";
const std::string kWrongSecret = "sec-test-WRONG-secret-key-32-bytes-long-bbbbbbb";

// Fast fake hasher (the production PBKDF2/bcrypt is too slow per-call for tests).
class FakePasswordHasher : public IPasswordHasher {
public:
    Result<std::string> Hash(const std::string& p) override { return std::string("fake$") + p; }
    Result<bool> Verify(const std::string& p, const std::string& h) override {
        return h == (std::string("fake$") + p);
    }
    int cost() const override { return 12; }
};

// A minimal valid access-token payload (far-future expiry unless overridden).
JwtPayload AccessPayload(const std::string& sub) {
    JwtPayload p;
    p.sub = sub;
    p.sid = "sess_sec";
    p.iat = 1;
    p.exp = 4102444800;  // year 2100
    p.email = "sec@example.com";
    p.tenant_id = "t_sec";
    p.role = "admin";
    return p;
}

// Build a forged compact JWS with an arbitrary header JSON + payload JSON, signed
// (or not) with `sig_secret`. If `sig_secret` is empty the signature segment is
// left empty (the alg=none "unsigned token" attack shape).
std::string ForgeToken(const std::string& header_json, const std::string& payload_json,
                       const std::string& sig_secret) {
    const std::string signing_input = JwtCodec::Base64UrlEncode(header_json) + "." +
                                      JwtCodec::Base64UrlEncode(payload_json);
    if (sig_secret.empty()) {
        return signing_input + ".";  // empty signature segment (alg=none style)
    }
    const std::string sig = JwtCodec::HmacSha256(sig_secret, signing_input);
    return signing_input + "." + JwtCodec::Base64UrlEncode(sig);
}

std::string AccessClaimsJson(const std::string& sub, int64_t exp) {
    json c;
    c["sub"] = sub;
    c["sid"] = "sess_sec";
    c["iat"] = 1;
    c["exp"] = exp;
    c["iss"] = "cortrix-auth";
    c["aud"] = "cortrix";
    c["email"] = "sec@example.com";
    c["tenant_id"] = "t_sec";
    c["role"] = "admin";
    return c.dump();
}

// =====================================================================
// MATRIX B (1): JwtCodec crypto-core attacks
// =====================================================================

// Positive control: a properly signed, unexpired token decodes.
TEST(SecJwtHardening, ValidToken_Accepted) {
    auto tok = JwtCodec::Encode(AccessPayload("usr_ok"), kSecret);
    ASSERT_TRUE(tok.ok());
    auto dec = JwtCodec::Decode(tok.value(), {kSecret}, /*now=*/100);
    ASSERT_TRUE(dec.ok()) << dec.status().message();
    EXPECT_EQ(dec.value().sub, "usr_ok");
}

// Expired token (exp in the past) -> rejected (CX_ERR_AUTH_TOKEN_EXPIRED).
TEST(SecJwtHardening, ExpiredToken_Rejected) {
    JwtPayload p = AccessPayload("usr_exp");
    p.exp = 1000;  // past
    auto tok = JwtCodec::Encode(p, kSecret);
    ASSERT_TRUE(tok.ok());
    auto dec = JwtCodec::Decode(tok.value(), {kSecret}, /*now=*/5000);
    ASSERT_FALSE(dec.ok()) << "expired token accepted -> VULNERABILITY";
    EXPECT_EQ(dec.status().code(), StatusCode::kUnauthenticated);
    EXPECT_NE(dec.status().message().find("CX_ERR_AUTH_TOKEN_EXPIRED"), std::string::npos);
}

// alg=none -> MUST be rejected (must not accept an unsigned token).
TEST(SecJwtHardening, AlgNone_Rejected) {
    const std::string header = R"({"alg":"none","typ":"JWT"})";
    const std::string tok = ForgeToken(header, AccessClaimsJson("usr_attacker", 4102444800),
                                       /*sig_secret=*/"");
    auto dec = JwtCodec::Decode(tok, {kSecret}, /*now=*/100);
    ASSERT_FALSE(dec.ok())
        << "alg=none token accepted -> CRITICAL VULNERABILITY (signature bypass)";
    EXPECT_EQ(dec.status().code(), StatusCode::kUnauthenticated);
}

// alg=none but with a signature segment present (some libs accept this) -> rejected.
TEST(SecJwtHardening, AlgNoneWithSignatureSegment_Rejected) {
    const std::string header = R"({"alg":"none","typ":"JWT"})";
    // Sign with the real secret but claim alg=none: the alg gate must still reject.
    const std::string tok = ForgeToken(header, AccessClaimsJson("usr_attacker", 4102444800),
                                       kSecret);
    auto dec = JwtCodec::Decode(tok, {kSecret}, /*now=*/100);
    ASSERT_FALSE(dec.ok()) << "alg=none (with sig) accepted -> VULNERABILITY";
    EXPECT_EQ(dec.status().code(), StatusCode::kUnauthenticated);
}

// Algorithm-confusion: claim a non-HS256 alg (e.g. RS256/HS512) -> rejected.
TEST(SecJwtHardening, AlgorithmConfusion_NonHs256_Rejected) {
    for (const char* alg : {"RS256", "HS512", "ES256", "HS384", "PS256"}) {
        const std::string header = std::string(R"({"alg":")") + alg + R"(","typ":"JWT"})";
        // Sign with the secret as if it were the HMAC key (the classic RS256->HS256
        // confusion shape where the public key is abused as an HMAC secret).
        const std::string tok = ForgeToken(header, AccessClaimsJson("usr_x", 4102444800), kSecret);
        auto dec = JwtCodec::Decode(tok, {kSecret}, /*now=*/100);
        ASSERT_FALSE(dec.ok())
            << "alg=" << alg << " accepted -> algorithm-confusion VULNERABILITY";
        EXPECT_EQ(dec.status().code(), StatusCode::kUnauthenticated);
    }
}

// Wrong signature (signed with a different secret) -> rejected.
TEST(SecJwtHardening, WrongSignature_Rejected) {
    auto tok = JwtCodec::Encode(AccessPayload("usr_y"), kWrongSecret);
    ASSERT_TRUE(tok.ok());
    auto dec = JwtCodec::Decode(tok.value(), {kSecret}, /*now=*/100);
    ASSERT_FALSE(dec.ok()) << "token signed with wrong secret accepted -> VULNERABILITY";
    EXPECT_EQ(dec.status().code(), StatusCode::kUnauthenticated);
    EXPECT_NE(dec.status().message().find("CX_ERR_UNAUTHORIZED"), std::string::npos);
}

// Tampered payload: keep a valid signature for the ORIGINAL payload but swap in an
// attacker-controlled payload segment (privilege escalation attempt) -> rejected
// because the signature no longer matches the new signing input.
TEST(SecJwtHardening, TamperedPayload_Rejected) {
    auto tok = JwtCodec::Encode(AccessPayload("usr_low"), kSecret);
    ASSERT_TRUE(tok.ok());
    const std::string& t = tok.value();
    const auto dot1 = t.find('.');
    const auto dot2 = t.find('.', dot1 + 1);
    ASSERT_NE(dot1, std::string::npos);
    ASSERT_NE(dot2, std::string::npos);

    // Forge a new payload (escalate role/sub) but reuse the original signature.
    const std::string evil_payload =
        JwtCodec::Base64UrlEncode(AccessClaimsJson("usr_root", 4102444800));
    const std::string header_seg = t.substr(0, dot1);
    const std::string sig_seg = t.substr(dot2 + 1);
    const std::string tampered = header_seg + "." + evil_payload + "." + sig_seg;

    auto dec = JwtCodec::Decode(tampered, {kSecret}, /*now=*/100);
    ASSERT_FALSE(dec.ok()) << "tampered payload accepted -> CRITICAL VULNERABILITY";
    EXPECT_EQ(dec.status().code(), StatusCode::kUnauthenticated);
}

// Garbage / structurally-malformed tokens -> rejected (not a crash, not accepted).
TEST(SecJwtHardening, MalformedStructure_Rejected) {
    const std::vector<std::string> bad = {
        "",                       // empty
        "notajwt",                // no dots
        "only.two",               // 2 segments
        "a.b.c.d",                // 4 segments
        "....",                   // empty segments
        "@@@.@@@.@@@",            // invalid base64url
    };
    for (const auto& t : bad) {
        auto dec = JwtCodec::Decode(t, {kSecret}, /*now=*/100);
        EXPECT_FALSE(dec.ok()) << "malformed token accepted: '" << t << "'";
    }
}

// =====================================================================
// MATRIX B (2): rotation invalidation + the real middleware decision path
// =====================================================================

class AuthServiceFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ::unsetenv("CORTRIX_JWT_SECRET");
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
    }
    AuthService Svc() {
        return AuthService(pdb_.db(), config::AuthConfig{}, &hasher_, kSecret);
    }
    PlatformDb pdb_;
    FakePasswordHasher hasher_;
};

// Positive control through the real middleware decision: register -> login ->
// the minted access token validates and yields the principal's identity.
TEST_F(AuthServiceFixture, ValidLoginToken_ValidatesThroughMiddleware) {
    AuthService svc = Svc();
    ASSERT_TRUE(svc.Register("live@example.com", "Secure123!", "Live").ok());
    auto login = svc.Login("live@example.com", "Secure123!");
    ASSERT_TRUE(login.ok()) << login.status().message();

    auto ctx = svc.ValidateAccessToken(login.value().access_token);
    ASSERT_TRUE(ctx.ok()) << ctx.status().message();
    EXPECT_EQ(ctx.value().email, "live@example.com");
    EXPECT_FALSE(ctx.value().user_id.empty());
}

// A garbage bearer through ValidateAccessToken -> unauthenticated (no crash).
TEST_F(AuthServiceFixture, GarbageToken_ValidateRejected) {
    AuthService svc = Svc();
    auto ctx = svc.ValidateAccessToken("not.a.valid.jwt.token");
    ASSERT_FALSE(ctx.ok());
    EXPECT_EQ(ctx.status().code(), StatusCode::kUnauthenticated);
}

// Rotation invalidation: a token signed with the OLD secret AFTER the rotation
// window closes (accept set = current only) MUST be rejected. Models S7 cron
// retiring the prev key — a stolen pre-rotation token must not survive forever.
TEST_F(AuthServiceFixture, OldSecretTokenAfterRotation_Rejected) {
    // Token signed with the old (now-retired) secret.
    auto old_tok = JwtCodec::Encode(AccessPayload("usr_old"), kSecret);
    ASSERT_TRUE(old_tok.ok());

    AuthService svc = Svc();
    // After the prev window passes, the accept set is the NEW secret only.
    svc.SetAcceptSecrets(/*sign=*/kWrongSecret, /*accept=*/{kWrongSecret});

    auto ctx = svc.ValidateAccessToken(old_tok.value());
    ASSERT_FALSE(ctx.ok())
        << "old-secret token still accepted after rotation -> VULNERABILITY (rotation "
           "does not invalidate)";
    EXPECT_EQ(ctx.status().code(), StatusCode::kUnauthenticated);
}

// During the dual-key window (accept = {new, old}) the old-secret token still
// verifies — positive control proving rotation does not over-reject mid-window.
TEST_F(AuthServiceFixture, OldSecretTokenDuringDualKeyWindow_Accepted) {
    auto old_tok = JwtCodec::Encode(AccessPayload("usr_window"), kSecret);
    ASSERT_TRUE(old_tok.ok());

    AuthService svc = Svc();
    svc.SetAcceptSecrets(/*sign=*/kWrongSecret, /*accept=*/{kWrongSecret, kSecret});

    auto ctx = svc.ValidateAccessToken(old_tok.value());
    ASSERT_TRUE(ctx.ok()) << ctx.status().message();
    EXPECT_EQ(ctx.value().user_id, "usr_window");
}

// NBF (not-before in the future): DEFERRED. JwtPayload has no nbf field and Decode
// does not enforce not-before (Cortrix never mints nbf). A forged nbf claim is an
// ignored extra field and, crucially, still requires a valid HS256 signature to be
// accepted at all — so it is not a forgeable bypass. We assert only the
// non-vulnerable corollary: a future-nbf token forged WITHOUT a valid signature is
// still rejected (by the signature gate), and document the nbf-unenforced design.
TEST(SecJwtHardening, NbfFutureForgedUnsigned_RejectedBySignatureGate_DeferredNote) {
    json c = json::parse(AccessClaimsJson("usr_nbf", 4102444800));
    c["nbf"] = 4102444800;  // not valid until year 2100 (ignored by Decode)
    const std::string header = R"({"alg":"HS256","typ":"JWT"})";
    // No valid signature available to an attacker -> sign with the wrong secret.
    const std::string tok = ForgeToken(header, c.dump(), kWrongSecret);
    auto dec = JwtCodec::Decode(tok, {kSecret}, /*now=*/100);
    EXPECT_FALSE(dec.ok())
        << "DEFERRED: nbf is not enforced; a future-nbf token with a forged "
           "signature must still be rejected by the signature gate";
    EXPECT_EQ(dec.status().code(), StatusCode::kUnauthenticated);
}

// =====================================================================
// MATRIX C: API-key validation + bootstrap token single-use
// =====================================================================

class ApiKeyFixture : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(pdb_.Open(":memory:").ok()); }
    PlatformDb pdb_;
};

// Non-existent key -> CX_ERR_AUTH_INVALID_API_KEY (401).
TEST_F(ApiKeyFixture, InvalidApiKey_NotFound_Rejected) {
    ApiKeyService svc(pdb_.db());
    auto r = svc.ValidateApiKey("cortrix_sk_does_not_exist");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kUnauthenticated);
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_INVALID_API_KEY"), std::string::npos);
}

// Revoked (disabled) key -> rejected.
TEST_F(ApiKeyFixture, RevokedApiKey_Rejected) {
    ApiKeyService svc(pdb_.db());
    // The api_keys.user_id FK targets users(id): seed a user first.
    ASSERT_EQ(sqlite3_exec(pdb_.db(),
                           "INSERT INTO users(id,email,password_hash,display_name,email_verified,"
                           "role,status,login_attempts,created_at,updated_at) "
                           "VALUES('usr_ak','ak@example.com','!x','AK',1,'admin','active',0,1,1)",
                           nullptr, nullptr, nullptr),
              SQLITE_OK)
        << sqlite3_errmsg(pdb_.db());
    auto created = svc.CreateApiKey("usr_ak", "test-key", /*expires_at_ms=*/0);
    ASSERT_TRUE(created.ok()) << created.status().message();
    // Valid before revoke (positive control).
    ASSERT_TRUE(svc.ValidateApiKey(created.value().plaintext).ok());

    ASSERT_TRUE(svc.RevokeApiKey(created.value().info.id).ok());
    auto after = svc.ValidateApiKey(created.value().plaintext);
    ASSERT_FALSE(after.ok()) << "revoked key still validated -> VULNERABILITY";
    EXPECT_EQ(after.status().code(), StatusCode::kUnauthenticated);
    EXPECT_NE(after.status().message().find("CX_ERR_AUTH_INVALID_API_KEY"), std::string::npos);
}

// Expired key (expires_at in the past, ms epoch) -> rejected.
TEST_F(ApiKeyFixture, ExpiredApiKey_Rejected) {
    ApiKeyService svc(pdb_.db());
    ASSERT_EQ(sqlite3_exec(pdb_.db(),
                           "INSERT INTO users(id,email,password_hash,display_name,email_verified,"
                           "role,status,login_attempts,created_at,updated_at) "
                           "VALUES('usr_ake','ake@example.com','!x','AKE',1,'admin','active',0,1,1)",
                           nullptr, nullptr, nullptr),
              SQLITE_OK)
        << sqlite3_errmsg(pdb_.db());
    // expires_at_ms=1 (1ms past epoch) -> already expired.
    auto created = svc.CreateApiKey("usr_ake", "expired-key", /*expires_at_ms=*/1);
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto r = svc.ValidateApiKey(created.value().plaintext);
    ASSERT_FALSE(r.ok()) << "expired key still validated -> VULNERABILITY";
    EXPECT_EQ(r.status().code(), StatusCode::kUnauthenticated);
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_INVALID_API_KEY"), std::string::npos);
}

// Bootstrap token single-use: a second Consume of the SAME token (reuse) and any
// Consume AFTER the token was consumed -> CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID.
TEST_F(ApiKeyFixture, BootstrapTokenReuse_Rejected) {
    ApiKeyService api_keys(pdb_.db());
    BootstrapHandler boot(pdb_.db(), &api_keys);

    // First start (empty users) -> a token is minted.
    auto first = boot.IsFirstStart();
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value());
    auto gen = boot.GenerateToken();
    ASSERT_TRUE(gen.ok()) << gen.status().message();
    const std::string token = gen.value();
    ASSERT_FALSE(token.empty());

    // First consume succeeds -> mints the one-time admin key.
    auto consumed = boot.Consume(token);
    ASSERT_TRUE(consumed.ok()) << consumed.status().message();
    EXPECT_FALSE(consumed.value().admin_api_key.empty());

    // Reuse of the SAME token -> rejected (single-use, token now consumed).
    auto reuse = boot.Consume(token);
    ASSERT_FALSE(reuse.ok()) << "bootstrap token reuse accepted -> CRITICAL VULNERABILITY";
    EXPECT_EQ(reuse.status().code(), StatusCode::kUnauthenticated);
    EXPECT_NE(reuse.status().message().find("CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID"),
              std::string::npos);
}

// An unknown / never-minted bootstrap token -> rejected.
TEST_F(ApiKeyFixture, BootstrapTokenUnknown_Rejected) {
    ApiKeyService api_keys(pdb_.db());
    BootstrapHandler boot(pdb_.db(), &api_keys);
    ASSERT_TRUE(boot.GenerateToken().ok());  // mint the real token
    auto r = boot.Consume("totally-wrong-bootstrap-token");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kUnauthenticated);
    EXPECT_NE(r.status().message().find("CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID"),
              std::string::npos);
}

}  // namespace
}  // namespace cortrix::auth
