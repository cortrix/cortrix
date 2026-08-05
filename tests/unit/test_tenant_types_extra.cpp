#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "cortrix/tenant/tenant_types.h"

// Exhaustive enum<->schema-token round-trips + unknown-token + cross-set
// disjointness for the tenant_types mapping. Basic round-trips live in
// test_tenant_types.cpp; this is the EXHAUSTIVE matrix incl. the disjointness
// invariant (TenantRole vs NsAclRole share tokens but are distinct enums) in
// NEW unique suites (TenantRoleParseMatrix / NsAclRoleParseMatrix /
// TenantTypeParseMatrix / QuotaLimitExtra).

namespace cortrix::tenant {
namespace {

// ---- TenantRole: token -> optional<role> (4 valid + rejects) -----------------

struct TenantRoleParseCase {
    std::string name;
    std::string token;
    bool expect_some;
    TenantRole expect;  // checked only when expect_some
};

class TenantRoleParseMatrix
    : public ::testing::TestWithParam<TenantRoleParseCase> {};

TEST_P(TenantRoleParseMatrix, Parse) {
    const auto& c = GetParam();
    std::optional<TenantRole> r = ParseTenantRole(c.token);
    EXPECT_EQ(r.has_value(), c.expect_some) << c.name;
    if (c.expect_some) {
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, c.expect) << c.name;
        // round-trip: ToString(parsed) == token
        EXPECT_EQ(std::string(ToString(*r)), c.token) << c.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Tenant, TenantRoleParseMatrix,
    ::testing::Values(
        TenantRoleParseCase{"owner", "owner", true, TenantRole::OWNER},
        TenantRoleParseCase{"admin", "admin", true, TenantRole::ADMIN},
        TenantRoleParseCase{"member", "member", true, TenantRole::MEMBER},
        TenantRoleParseCase{"viewer", "viewer", true, TenantRole::VIEWER},
        // "editor" is an NsAclRole token, NOT a TenantRole -> rejected here.
        TenantRoleParseCase{"editor_not_tenant_role", "editor", false, TenantRole::OWNER},
        TenantRoleParseCase{"uppercase_bad", "OWNER", false, TenantRole::OWNER},
        TenantRoleParseCase{"empty_bad", "", false, TenantRole::OWNER},
        TenantRoleParseCase{"unknown_bad", "superuser", false, TenantRole::OWNER}),
    [](const ::testing::TestParamInfo<TenantRoleParseCase>& i) { return i.param.name; });

// ---- NsAclRole: token -> optional<role> (3 valid + rejects) ------------------

struct NsAclRoleParseCase {
    std::string name;
    std::string token;
    bool expect_some;
    NsAclRole expect;
};

class NsAclRoleParseMatrix
    : public ::testing::TestWithParam<NsAclRoleParseCase> {};

TEST_P(NsAclRoleParseMatrix, Parse) {
    const auto& c = GetParam();
    std::optional<NsAclRole> r = ParseNsAclRole(c.token);
    EXPECT_EQ(r.has_value(), c.expect_some) << c.name;
    if (c.expect_some) {
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, c.expect) << c.name;
        EXPECT_EQ(std::string(ToString(*r)), c.token) << c.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    NsAcl, NsAclRoleParseMatrix,
    ::testing::Values(
        NsAclRoleParseCase{"viewer", "viewer", true, NsAclRole::VIEWER},
        NsAclRoleParseCase{"editor", "editor", true, NsAclRole::EDITOR},
        NsAclRoleParseCase{"admin", "admin", true, NsAclRole::ADMIN},
        // "owner"/"member" are TenantRole tokens, NOT NsAclRole -> rejected.
        NsAclRoleParseCase{"owner_not_nsacl", "owner", false, NsAclRole::VIEWER},
        NsAclRoleParseCase{"member_not_nsacl", "member", false, NsAclRole::VIEWER},
        NsAclRoleParseCase{"empty_bad", "", false, NsAclRole::VIEWER},
        NsAclRoleParseCase{"unknown_bad", "contributor", false, NsAclRole::VIEWER}),
    [](const ::testing::TestParamInfo<NsAclRoleParseCase>& i) { return i.param.name; });

// ---- TenantType round-trip ---------------------------------------------------

struct TenantTypeParseCase {
    std::string name;
    std::string token;
    bool expect_some;
    TenantType expect;
};

class TenantTypeParseMatrix
    : public ::testing::TestWithParam<TenantTypeParseCase> {};

TEST_P(TenantTypeParseMatrix, Parse) {
    const auto& c = GetParam();
    std::optional<TenantType> r = ParseTenantType(c.token);
    EXPECT_EQ(r.has_value(), c.expect_some) << c.name;
    if (c.expect_some) {
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, c.expect) << c.name;
        EXPECT_EQ(std::string(ToString(*r)), c.token) << c.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Type, TenantTypeParseMatrix,
    ::testing::Values(
        TenantTypeParseCase{"personal", "personal", true, TenantType::PERSONAL},
        TenantTypeParseCase{"organization", "organization", true, TenantType::ORGANIZATION},
        TenantTypeParseCase{"system", "system", true, TenantType::SYSTEM},
        TenantTypeParseCase{"org_short_bad", "org", false, TenantType::PERSONAL},
        TenantTypeParseCase{"empty_bad", "", false, TenantType::PERSONAL},
        TenantTypeParseCase{"unknown_bad", "team", false, TenantType::PERSONAL}),
    [](const ::testing::TestParamInfo<TenantTypeParseCase>& i) { return i.param.name; });

// ---- QuotaType ToString stability (all five tokens) --------------------------

TEST(QuotaTypeExtra, AllFiveTokensStable) {
    EXPECT_EQ(std::string(ToString(QuotaType::MAX_NAMESPACES)), "max_namespaces");
    EXPECT_EQ(std::string(ToString(QuotaType::MAX_DOCUMENTS_PER_NS)), "max_documents_per_ns");
    EXPECT_EQ(std::string(ToString(QuotaType::MAX_STORAGE_BYTES)), "max_storage_bytes");
    EXPECT_EQ(std::string(ToString(QuotaType::MAX_QPS)), "max_qps");
    EXPECT_EQ(std::string(ToString(QuotaType::MAX_MEMBERS_PER_TENANT)), "max_members_per_tenant");
}

// ---- QuotaLimit unlimited semantics ------------------------------------------

struct QuotaLimitCase {
    std::string name;
    int64_t limit;
    bool expect_unlimited;
};

class QuotaLimitExtra : public ::testing::TestWithParam<QuotaLimitCase> {};

TEST_P(QuotaLimitExtra, IsUnlimited) {
    const auto& c = GetParam();
    QuotaLimit q{c.limit};
    EXPECT_EQ(q.IsUnlimited(), c.expect_unlimited) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Quota, QuotaLimitExtra,
    ::testing::Values(
        QuotaLimitCase{"neg_one_unlimited", -1, true},
        QuotaLimitCase{"large_neg_unlimited", -1000000, true},
        QuotaLimitCase{"zero_limited", 0, false},
        QuotaLimitCase{"one_limited", 1, false},
        QuotaLimitCase{"large_limited", 1000000, false}),
    [](const ::testing::TestParamInfo<QuotaLimitCase>& i) { return i.param.name; });

TEST(QuotaLimitExtraDirect, FactoryUnlimited) {
    QuotaLimit u = QuotaLimit::Unlimited();
    EXPECT_TRUE(u.IsUnlimited());
    EXPECT_LT(u.limit, 0);
}

}  // namespace
}  // namespace cortrix::tenant
