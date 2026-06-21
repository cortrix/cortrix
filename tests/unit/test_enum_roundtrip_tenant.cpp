#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "cortrix/tenant/tenant_types.h"

// Exhaustive enum <-> physical-schema-string round-trip matrices for the tenant
// types (src/tenant/tenant_types.cpp). One TEST_P case per enum value across
// four enums, plus per-enum unknown-token -> documented default (nullopt for the
// three Parse* functions). Suite names are globally unique (RtTenant* prefix);
// these are intentionally NEW exhaustive sweeps distinct from the existing
// hand-written TenantTypesTest / *ParseMatrix cases.
namespace cortrix::tenant {
namespace {

// ----------------------------------------------------------------------------
// TenantRole: ToString -> ParseTenantRole round-trip over all 4 values.
// ----------------------------------------------------------------------------
const std::vector<TenantRole>& AllTenantRoles() {
    static const std::vector<TenantRole> v = {
        TenantRole::OWNER, TenantRole::ADMIN, TenantRole::MEMBER,
        TenantRole::VIEWER};
    return v;
}

class RtTenantRoleMatrix : public ::testing::TestWithParam<TenantRole> {};

TEST_P(RtTenantRoleMatrix, ToStringParseRoundTrip) {
    const TenantRole role = GetParam();
    const std::string token = ToString(role);
    EXPECT_FALSE(token.empty());
    const std::optional<TenantRole> parsed = ParseTenantRole(token);
    ASSERT_TRUE(parsed.has_value()) << "token did not parse: " << token;
    EXPECT_EQ(*parsed, role) << "round-trip mismatch for token: " << token;
}

TEST_P(RtTenantRoleMatrix, TokenIsLowercaseAscii) {
    const std::string token = ToString(GetParam());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_')
            << "non-lowercase token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtTenantRoleMatrix,
                         ::testing::ValuesIn(AllTenantRoles()));

// Exact token contract (the physical schema strings stored in user_tenants.role).
TEST(RtTenantRoleTokens, ExactSchemaStrings) {
    EXPECT_STREQ(ToString(TenantRole::OWNER), "owner");
    EXPECT_STREQ(ToString(TenantRole::ADMIN), "admin");
    EXPECT_STREQ(ToString(TenantRole::MEMBER), "member");
    EXPECT_STREQ(ToString(TenantRole::VIEWER), "viewer");
}

// Unknown token -> std::nullopt (documented: "Returns std::nullopt for an
// unknown token"). Empty / wrong-case / cross-enum tokens all reject.
class RtTenantRoleUnknownMatrix
    : public ::testing::TestWithParam<std::string> {};

TEST_P(RtTenantRoleUnknownMatrix, UnknownTokenIsNullopt) {
    EXPECT_FALSE(ParseTenantRole(GetParam()).has_value())
        << "unexpectedly parsed: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Unknowns, RtTenantRoleUnknownMatrix,
    ::testing::Values(std::string(""), "OWNER", "Owner", "root", "editor",
                      "personal", " owner", "owner "));

// ----------------------------------------------------------------------------
// NsAclRole: disjoint from TenantRole on purpose. 3 values.
// ----------------------------------------------------------------------------
const std::vector<NsAclRole>& AllNsAclRoles() {
    static const std::vector<NsAclRole> v = {
        NsAclRole::VIEWER, NsAclRole::EDITOR, NsAclRole::ADMIN};
    return v;
}

class RtNsAclRoleMatrix : public ::testing::TestWithParam<NsAclRole> {};

TEST_P(RtNsAclRoleMatrix, ToStringParseRoundTrip) {
    const NsAclRole role = GetParam();
    const std::string token = ToString(role);
    EXPECT_FALSE(token.empty());
    const std::optional<NsAclRole> parsed = ParseNsAclRole(token);
    ASSERT_TRUE(parsed.has_value()) << "token did not parse: " << token;
    EXPECT_EQ(*parsed, role) << "round-trip mismatch for token: " << token;
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtNsAclRoleMatrix,
                         ::testing::ValuesIn(AllNsAclRoles()));

TEST(RtNsAclRoleTokens, ExactSchemaStrings) {
    EXPECT_STREQ(ToString(NsAclRole::VIEWER), "viewer");
    EXPECT_STREQ(ToString(NsAclRole::EDITOR), "editor");
    EXPECT_STREQ(ToString(NsAclRole::ADMIN), "admin");
}

// NsAclRole has no MEMBER/OWNER -> those TenantRole tokens must reject here.
class RtNsAclRoleUnknownMatrix
    : public ::testing::TestWithParam<std::string> {};

TEST_P(RtNsAclRoleUnknownMatrix, UnknownTokenIsNullopt) {
    EXPECT_FALSE(ParseNsAclRole(GetParam()).has_value())
        << "unexpectedly parsed: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtNsAclRoleUnknownMatrix,
                         ::testing::Values(std::string(""), "owner", "member",
                                           "EDITOR", "Editor", "admins"));

// ----------------------------------------------------------------------------
// TenantType: 3 values (tenants.type column tokens).
// ----------------------------------------------------------------------------
const std::vector<TenantType>& AllTenantTypes() {
    static const std::vector<TenantType> v = {
        TenantType::PERSONAL, TenantType::ORGANIZATION, TenantType::SYSTEM};
    return v;
}

class RtTenantTypeMatrix : public ::testing::TestWithParam<TenantType> {};

TEST_P(RtTenantTypeMatrix, ToStringParseRoundTrip) {
    const TenantType type = GetParam();
    const std::string token = ToString(type);
    EXPECT_FALSE(token.empty());
    const std::optional<TenantType> parsed = ParseTenantType(token);
    ASSERT_TRUE(parsed.has_value()) << "token did not parse: " << token;
    EXPECT_EQ(*parsed, type) << "round-trip mismatch for token: " << token;
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtTenantTypeMatrix,
                         ::testing::ValuesIn(AllTenantTypes()));

TEST(RtTenantTypeTokens, ExactSchemaStrings) {
    EXPECT_STREQ(ToString(TenantType::PERSONAL), "personal");
    EXPECT_STREQ(ToString(TenantType::ORGANIZATION), "organization");
    EXPECT_STREQ(ToString(TenantType::SYSTEM), "system");
}

class RtTenantTypeUnknownMatrix
    : public ::testing::TestWithParam<std::string> {};

TEST_P(RtTenantTypeUnknownMatrix, UnknownTokenIsNullopt) {
    EXPECT_FALSE(ParseTenantType(GetParam()).has_value())
        << "unexpectedly parsed: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtTenantTypeUnknownMatrix,
                         ::testing::Values(std::string(""), "org", "Personal",
                                           "PERSONAL", "user", "team"));

// ----------------------------------------------------------------------------
// QuotaType: ToString only (no Parse). Assert stable tokens + uniqueness across
// all 5 values. (One-way enum: no round-trip surface, so we pin the contract.)
// ----------------------------------------------------------------------------
const std::vector<QuotaType>& AllQuotaTypes() {
    static const std::vector<QuotaType> v = {
        QuotaType::MAX_NAMESPACES, QuotaType::MAX_DOCUMENTS_PER_NS,
        QuotaType::MAX_STORAGE_BYTES, QuotaType::MAX_QPS,
        QuotaType::MAX_MEMBERS_PER_TENANT};
    return v;
}

class RtQuotaTypeMatrix : public ::testing::TestWithParam<QuotaType> {};

TEST_P(RtQuotaTypeMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_')
            << "non-lowercase token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtQuotaTypeMatrix,
                         ::testing::ValuesIn(AllQuotaTypes()));

TEST(RtQuotaTypeTokens, ExactSchemaStringsAndUniqueness) {
    EXPECT_STREQ(ToString(QuotaType::MAX_NAMESPACES), "max_namespaces");
    EXPECT_STREQ(ToString(QuotaType::MAX_DOCUMENTS_PER_NS),
                 "max_documents_per_ns");
    EXPECT_STREQ(ToString(QuotaType::MAX_STORAGE_BYTES), "max_storage_bytes");
    EXPECT_STREQ(ToString(QuotaType::MAX_QPS), "max_qps");
    EXPECT_STREQ(ToString(QuotaType::MAX_MEMBERS_PER_TENANT),
                 "max_members_per_tenant");
}

}  // namespace
}  // namespace cortrix::tenant
