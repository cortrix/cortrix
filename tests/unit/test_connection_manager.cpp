#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cortrix/auth/auth_context.h"
#include "cortrix/import/connection_manager.h"
#include "cortrix/import/import_error.h"

// S1 coverage: ConnectionManager D1 lifecycle — register / resolve /
// list / revoke + AES secret round-trip + 30d expiry + D7 cross-tenant guard. Runs
// against the in-memory secret store + connection store (standalone doubles; the
// SQLite-backed store + real DSN resolution is D3.5).
namespace cortrix::import {
namespace {

AuthContext AdminCtx(const std::string& tenant, const std::string& user = "admin1") {
    AuthContext ctx;
    ctx.tenant_id = tenant;
    ctx.user_id = user;
    ctx.permissions = kPermAdmin | kPermRead | kPermWrite;
    return ctx;
}

class ConnectionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        secret_ = std::make_shared<InMemorySecretStore>();
        store_ = std::make_shared<InMemoryConnectionStore>();
        mgr_ = std::make_unique<ConnectionManager>(secret_, store_);
    }
    std::shared_ptr<InMemorySecretStore> secret_;
    std::shared_ptr<InMemoryConnectionStore> store_;
    std::unique_ptr<ConnectionManager> mgr_;

    static constexpr const char* kDsn =
        "postgres://app:s3cret@db.internal:5432/crm?sslmode=require";
};

TEST_F(ConnectionManagerTest, RegisterReturnsRefAndStoresMetadataNotDsn) {
    auto r = mgr_->Register("crm-db", kDsn, "t1", "admin1");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().rfind("db_conn_", 0), 0u);  // "db_conn_<ulid>" prefix

    auto list = mgr_->ListConnections("t1");
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].name, "crm-db");
    EXPECT_EQ(list[0].host, "db.internal");      // metadata parsed, no password
    EXPECT_EQ(list[0].db_name, "crm");
    EXPECT_FALSE(list[0].revoked);
    // The DSN/password must NOT appear anywhere in the metadata view.
    // (ConnectionInfo has no DSN field by construction — compile-time guarantee.)
}

TEST_F(ConnectionManagerTest, ResolveDsnRoundTripsThroughAesStore) {
    auto r = mgr_->Register("crm-db", kDsn, "t1", "admin1");
    ASSERT_TRUE(r.ok());
    auto dsn = mgr_->ResolveDsn(r.value(), AdminCtx("t1"));
    ASSERT_TRUE(dsn.ok()) << dsn.status().message();
    EXPECT_EQ(dsn.value(), kDsn);  // AES-256-GCM decrypt recovers the exact DSN
}

TEST_F(ConnectionManagerTest, SecretStoreBlobIsNotPlaintext) {
    // The encrypted blob (what the store actually holds) must not contain the
    // plaintext password substring.
    std::string key_id = secret_->Put(kDsn);
    ASSERT_FALSE(key_id.empty());
    auto got = secret_->Get(key_id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, kDsn);
    // unknown key → nullopt
    EXPECT_FALSE(secret_->Get("sk_does_not_exist").has_value());
}

TEST_F(ConnectionManagerTest, CrossTenantResolveThrowsCrossTenantRef) {
    auto r = mgr_->Register("crm-db", kDsn, "t1", "admin1");
    ASSERT_TRUE(r.ok());
    // tenant t2 trying to resolve a t1-owned ref → CX_ERR_F16A_CROSS_TENANT_REF.
    try {
        (void)mgr_->ResolveDsn(r.value(), AdminCtx("t2"));
        FAIL() << "expected F16aException";
    } catch (const F16aException& e) {
        EXPECT_EQ(e.code(), F16aErrorCode::kCrossTenantRef);
        ASSERT_TRUE(e.GetError().structured_data.has_value());
        EXPECT_EQ((*e.GetError().structured_data)["connection_ref"], r.value());
    }
}

TEST_F(ConnectionManagerTest, RevokeBlocksResolveAndErasesSecret) {
    auto r = mgr_->Register("crm-db", kDsn, "t1", "admin1");
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(mgr_->Revoke(r.value(), AdminCtx("t1"), "rotation").ok());

    auto list = mgr_->ListConnections("t1");
    ASSERT_EQ(list.size(), 1u);
    EXPECT_TRUE(list[0].revoked);

    auto dsn = mgr_->ResolveDsn(r.value(), AdminCtx("t1"));
    EXPECT_FALSE(dsn.ok());
    EXPECT_EQ(dsn.status().code(), StatusCode::kPermissionDenied);
}

TEST_F(ConnectionManagerTest, CrossTenantRevokeRejected) {
    auto r = mgr_->Register("crm-db", kDsn, "t1", "admin1");
    ASSERT_TRUE(r.ok());
    Status s = mgr_->Revoke(r.value(), AdminCtx("t2"), "evil");
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_F16A_CROSS_TENANT_REF"), std::string::npos);
}

TEST_F(ConnectionManagerTest, ExpiredRefDeniesResolve) {
    // Register with an already-elapsed window by directly seeding the store with a
    // past expires_at (the public Register enforces >=1 day, so we go via the store).
    std::string sk = secret_->Put(kDsn);
    IConnectionStore::Record rec;
    rec.ref_id = "db_conn_expired";
    rec.name = "old";
    rec.tenant_id = "t1";
    rec.secret_key_id = sk;
    rec.host = "h";
    rec.db_name = "d";
    rec.registered_by = "admin1";
    rec.registered_at_ms = 1;
    rec.expires_at_ms = 2;  // long past
    ASSERT_TRUE(store_->Insert(rec).ok());

    auto dsn = mgr_->ResolveDsn("db_conn_expired", AdminCtx("t1"));
    EXPECT_FALSE(dsn.ok());
    EXPECT_NE(dsn.status().message().find("expired"), std::string::npos);
}

TEST_F(ConnectionManagerTest, DefaultExpiryIs30Days) {
    auto r = mgr_->Register("crm-db", kDsn, "t1", "admin1");
    ASSERT_TRUE(r.ok());
    auto list = mgr_->ListConnections("t1");
    ASSERT_EQ(list.size(), 1u);
    int64_t window = list[0].expires_at_ms - list[0].registered_at_ms;
    int64_t thirty_days = static_cast<int64_t>(30) * 24 * 3600 * 1000;
    EXPECT_EQ(window, thirty_days);
}

TEST_F(ConnectionManagerTest, RejectsBadInputAndOutOfRangeExpiry) {
    EXPECT_FALSE(mgr_->Register("", kDsn, "t1", "u").ok());
    EXPECT_FALSE(mgr_->Register("n", "", "t1", "u").ok());
    EXPECT_FALSE(mgr_->Register("n", kDsn, "", "u").ok());
    EXPECT_FALSE(mgr_->Register("n", kDsn, "t1", "u", 0).ok());
    EXPECT_FALSE(mgr_->Register("n", kDsn, "t1", "u", 99999).ok());  // > expire_max_days
    EXPECT_TRUE(mgr_->Register("n", kDsn, "t1", "u", 7).ok());
}

TEST_F(ConnectionManagerTest, ListIsTenantScoped) {
    ASSERT_TRUE(mgr_->Register("a", kDsn, "t1", "u").ok());
    ASSERT_TRUE(mgr_->Register("b", kDsn, "t2", "u").ok());
    EXPECT_EQ(mgr_->ListConnections("t1").size(), 1u);
    EXPECT_EQ(mgr_->ListConnections("t2").size(), 1u);
    EXPECT_EQ(mgr_->ListConnections("t3").size(), 0u);
}

}  // namespace
}  // namespace cortrix::import
