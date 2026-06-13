// P09 sec 4.1 -- enum <-> physical-schema string mapping round-trips (S1).
#include <gtest/gtest.h>

#include <string>

#include "cortrix/tenant/tenant_types.h"

using namespace cortrix::tenant;

TEST(TenantTypesTest, TenantRoleStringsMatchSchema) {
    EXPECT_STREQ(ToString(TenantRole::OWNER), "owner");
    EXPECT_STREQ(ToString(TenantRole::ADMIN), "admin");
    EXPECT_STREQ(ToString(TenantRole::MEMBER), "member");
    EXPECT_STREQ(ToString(TenantRole::VIEWER), "viewer");
}

TEST(TenantTypesTest, TenantRoleRoundTrips) {
    for (auto r : {TenantRole::OWNER, TenantRole::ADMIN, TenantRole::MEMBER, TenantRole::VIEWER}) {
        auto parsed = ParseTenantRole(ToString(r));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, r);
    }
    EXPECT_FALSE(ParseTenantRole("superuser").has_value());
    EXPECT_FALSE(ParseTenantRole("").has_value());
}

TEST(TenantTypesTest, NsAclRoleRoundTrips) {
    for (auto r : {NsAclRole::VIEWER, NsAclRole::EDITOR, NsAclRole::ADMIN}) {
        auto parsed = ParseNsAclRole(ToString(r));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, r);
    }
    // NsAclRole has no MEMBER/OWNER -- distinct ladder from TenantRole.
    EXPECT_FALSE(ParseNsAclRole("owner").has_value());
    EXPECT_FALSE(ParseNsAclRole("member").has_value());
}

TEST(TenantTypesTest, TenantTypeRoundTrips) {
    for (auto t : {TenantType::PERSONAL, TenantType::ORGANIZATION, TenantType::SYSTEM}) {
        auto parsed = ParseTenantType(ToString(t));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, t);
    }
    EXPECT_FALSE(ParseTenantType("team").has_value());
}

TEST(TenantTypesTest, QuotaLimitUnlimitedSemantics) {
    auto u = QuotaLimit::Unlimited();
    EXPECT_TRUE(u.IsUnlimited());
    EXPECT_LT(u.limit, 0);

    QuotaLimit bounded{100};
    EXPECT_FALSE(bounded.IsUnlimited());
    EXPECT_EQ(bounded.limit, 100);

    QuotaLimit zero{0};
    EXPECT_FALSE(zero.IsUnlimited());  // 0 is a real (zero) limit, not unlimited
}

TEST(TenantTypesTest, QuotaTypeStringsStable) {
    EXPECT_STREQ(ToString(QuotaType::MAX_NAMESPACES), "max_namespaces");
    EXPECT_STREQ(ToString(QuotaType::MAX_DOCUMENTS_PER_NS), "max_documents_per_ns");
    EXPECT_STREQ(ToString(QuotaType::MAX_STORAGE_BYTES), "max_storage_bytes");
    EXPECT_STREQ(ToString(QuotaType::MAX_QPS), "max_qps");
    EXPECT_STREQ(ToString(QuotaType::MAX_MEMBERS_PER_TENANT), "max_members_per_tenant");
}
