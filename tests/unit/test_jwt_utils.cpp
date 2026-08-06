#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/auth/auth_error.h"
#include "cortrix/auth/jwt_utils.h"

// Auth S3 coverage: HS256 JWT codec (OpenSSL HMAC-SHA256, team-lead approved
// 2026-05-31). Round-trip / expiry / signature / type.
namespace cortrix::auth {
namespace {

const std::string kSecret = "test-secret-key-at-least-32-bytes-long-for-testing";
constexpr int64_t kNow = 1712035200;  // fixed "now" for deterministic exp checks

JwtPayload MakeAccess() {
    JwtPayload p;
    p.sub = "usr_a1b2c3d4";
    p.email = "user@example.com";
    p.tenant_id = "tenant-uuid";
    p.role = "owner";
    p.sid = "sess_x1y2z3";
    p.iat = kNow;
    p.exp = kNow + 3600;
    return p;
}

// base64url is the RFC 7515 alphabet: no padding, '-'/'_' (never '+'/'/'/'=').
TEST(JwtBase64UrlTest, RoundTripAndAlphabet) {
    for (const auto& s : {std::string(""), std::string("f"), std::string("fo"),
                          std::string("foo"), std::string("foob"),
                          std::string("hello, world!"),
                          std::string("\x00\xff\x10\xab", 4)}) {
        const std::string enc = JwtCodec::Base64UrlEncode(s);
        EXPECT_EQ(enc.find('+'), std::string::npos);
        EXPECT_EQ(enc.find('/'), std::string::npos);
        EXPECT_EQ(enc.find('='), std::string::npos);
        auto dec = JwtCodec::Base64UrlDecode(enc);
        ASSERT_TRUE(dec.ok());
        EXPECT_EQ(dec.value(), s);
    }
}

TEST(JwtUtilsTest, GenerateAndDecodeRoundTrip) {
    auto tok = JwtCodec::Encode(MakeAccess(), kSecret);
    ASSERT_TRUE(tok.ok()) << tok.status().message();

    auto dec = JwtCodec::Decode(tok.value(), {kSecret}, kNow);
    ASSERT_TRUE(dec.ok()) << dec.status().message();
    EXPECT_EQ(dec.value().sub, "usr_a1b2c3d4");
    EXPECT_EQ(dec.value().email, "user@example.com");
    EXPECT_EQ(dec.value().tenant_id, "tenant-uuid");
    EXPECT_EQ(dec.value().role, "owner");
    EXPECT_EQ(dec.value().sid, "sess_x1y2z3");
    EXPECT_EQ(dec.value().iss, "cortrix-auth");  // Issue 3.3 placeholders
    EXPECT_EQ(dec.value().aud, "cortrix");
    EXPECT_FALSE(dec.value().is_refresh());
}

TEST(JwtUtilsTest, ExpiredToken) {
    JwtPayload p = MakeAccess();
    p.exp = kNow - 1;  // already expired
    auto tok = JwtCodec::Encode(p, kSecret);
    ASSERT_TRUE(tok.ok());
    auto dec = JwtCodec::Decode(tok.value(), {kSecret}, kNow);
    ASSERT_FALSE(dec.ok());
    EXPECT_NE(dec.status().message().find("CX_ERR_AUTH_TOKEN_EXPIRED"), std::string::npos);
}

TEST(JwtUtilsTest, InvalidSignatureTampered) {
    auto tok = JwtCodec::Encode(MakeAccess(), kSecret);
    ASSERT_TRUE(tok.ok());
    std::string t = tok.value();
    t[t.size() - 2] = (t[t.size() - 2] == 'A') ? 'B' : 'A';  // flip a sig char
    auto dec = JwtCodec::Decode(t, {kSecret}, kNow);
    ASSERT_FALSE(dec.ok());
    EXPECT_NE(dec.status().message().find("CX_ERR_UNAUTHORIZED"), std::string::npos);
}

TEST(JwtUtilsTest, WrongSecret) {
    auto tok = JwtCodec::Encode(MakeAccess(), kSecret);
    ASSERT_TRUE(tok.ok());
    auto dec = JwtCodec::Decode(tok.value(), {"a-different-secret-of-sufficient-length!"}, kNow);
    ASSERT_FALSE(dec.ok());
    EXPECT_EQ(dec.status().code(), StatusCode::kUnauthenticated);
}

// Dual-key window (S7 forward-compat): a token signed with the prev secret still
// verifies when prev is in the accept list.
TEST(JwtUtilsTest, AcceptsAnySecretInList) {
    const std::string prev = "previous-secret-key-also-32-bytes-long-xx";
    auto tok = JwtCodec::Encode(MakeAccess(), prev);
    ASSERT_TRUE(tok.ok());
    // current first, prev second — must still accept the prev-signed token.
    auto dec = JwtCodec::Decode(tok.value(), {kSecret, prev}, kNow);
    ASSERT_TRUE(dec.ok()) << dec.status().message();
    EXPECT_EQ(dec.value().sub, "usr_a1b2c3d4");
}

TEST(JwtUtilsTest, AccessTokenType) {
    auto dec = JwtCodec::Decode(JwtCodec::Encode(MakeAccess(), kSecret).value(), {kSecret}, kNow);
    ASSERT_TRUE(dec.ok());
    EXPECT_FALSE(dec.value().is_refresh());
    EXPECT_TRUE(dec.value().type.empty());
}

TEST(JwtUtilsTest, RefreshTokenType) {
    JwtPayload r;
    r.sub = "usr_a1b2c3d4";
    r.sid = "sess_x1y2z3";
    r.type = "refresh";
    r.jti = "ref_x1y2z3";
    r.iat = kNow;
    r.exp = kNow + 2592000;
    auto tok = JwtCodec::Encode(r, kSecret);
    ASSERT_TRUE(tok.ok());
    auto dec = JwtCodec::Decode(tok.value(), {kSecret}, kNow);
    ASSERT_TRUE(dec.ok());
    EXPECT_TRUE(dec.value().is_refresh());
    EXPECT_EQ(dec.value().jti, "ref_x1y2z3");
}

TEST(JwtUtilsTest, MalformedStructureRejected) {
    for (const auto* bad : {"", "onlyonesegment", "two.segments",
                            "a.b.c.d", "....", "not-base64!!.x.y"}) {
        auto dec = JwtCodec::Decode(bad, {kSecret}, kNow);
        EXPECT_FALSE(dec.ok()) << "should reject: " << bad;
    }
}

TEST(JwtUtilsTest, HmacIsDeterministicAnd32Bytes) {
    const std::string a = JwtCodec::HmacSha256(kSecret, "message");
    const std::string b = JwtCodec::HmacSha256(kSecret, "message");
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.size(), 32u);  // SHA-256 → 32 bytes
    EXPECT_NE(a, JwtCodec::HmacSha256(kSecret, "message2"));
    EXPECT_NE(a, JwtCodec::HmacSha256("other-secret", "message"));
}

}  // namespace
}  // namespace cortrix::auth
