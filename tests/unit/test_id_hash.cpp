#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "cortrix/auth/platform_db.h"
#include "cortrix/id/hash.h"
#include "cortrix/util/siphash.h"

// S2 (ID system): LoadOrBootstrapHashKey over platform.db auth_secrets
// (secret_type='siphash_id_key'), mirroring P08 JwtSecretService::LoadOrInit, plus
// the HashChildIdToBlockId contract. Test layout follows test_auth_config_init.cpp.
namespace cortrix::id {
namespace {

int CurrentKeyRows(sqlite3* db, const char* where) {
    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("SELECT COUNT(*) FROM auth_secrets WHERE ") + where;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK);
    int n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

// Fresh platform.db → bootstrap writes one current siphash_id_key and sets the
// process-global key to a (non-zero) value.
TEST(IdHashKeyTest, BootstrapFirstStart) {
    ::unsetenv("CORTRIX_SIPHASH_ID_KEY");
    kDeploymentHashKey = SipHashKey{};  // reset global
    auth::PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());

    ASSERT_TRUE(LoadOrBootstrapHashKey(pdb.db()).ok());
    EXPECT_EQ(CurrentKeyRows(pdb.db(),
                             "secret_type='siphash_id_key' AND status='current'"),
              1);
    // A random 128-bit key being exactly zero is astronomically unlikely.
    EXPECT_FALSE(kDeploymentHashKey.k0 == 0 && kDeploymentHashKey.k1 == 0);
}

// A second load over the same on-disk db yields the SAME key and adds NO new row —
// persistence is the property the P-HNSW index relies on (ARCH §1.8.2).
TEST(IdHashKeyTest, LoadExistingAcrossRestart) {
    ::unsetenv("CORTRIX_SIPHASH_ID_KEY");
    const std::string path = std::string(::testing::TempDir()) + "id_siphash_restart.db";
    std::remove(path.c_str());

    SipHashKey first;
    {
        auth::PlatformDb p1;
        ASSERT_TRUE(p1.Open(path).ok());
        ASSERT_TRUE(LoadOrBootstrapHashKey(p1.db()).ok());
        first = kDeploymentHashKey;
        EXPECT_EQ(CurrentKeyRows(p1.db(), "secret_type='siphash_id_key'"), 1);
    }
    kDeploymentHashKey = SipHashKey{};  // clear → prove step (2) reloads from db
    {
        auth::PlatformDb p2;
        ASSERT_TRUE(p2.Open(path).ok());
        ASSERT_TRUE(LoadOrBootstrapHashKey(p2.db()).ok());
        EXPECT_EQ(kDeploymentHashKey.k0, first.k0);
        EXPECT_EQ(kDeploymentHashKey.k1, first.k1);
        EXPECT_EQ(CurrentKeyRows(p2.db(), "secret_type='siphash_id_key'"), 1);  // no new row
    }
    std::remove(path.c_str());
}

// CORTRIX_SIPHASH_ID_KEY (32 hex) overrides + is written through as the sole current.
// Bytes 00..0f little-endian → k0=0x0706050403020100, k1=0x0f0e0d0c0b0a0908.
TEST(IdHashKeyTest, EnvOverride) {
    const std::string path = std::string(::testing::TempDir()) + "id_siphash_env.db";
    std::remove(path.c_str());
    {
        ::unsetenv("CORTRIX_SIPHASH_ID_KEY");
        auth::PlatformDb p0;
        ASSERT_TRUE(p0.Open(path).ok());
        ASSERT_TRUE(LoadOrBootstrapHashKey(p0.db()).ok());  // seed an auto key
    }
    ::setenv("CORTRIX_SIPHASH_ID_KEY", "000102030405060708090a0b0c0d0e0f", /*overwrite=*/1);
    {
        auth::PlatformDb p1;
        ASSERT_TRUE(p1.Open(path).ok());
        ASSERT_TRUE(LoadOrBootstrapHashKey(p1.db()).ok());
        EXPECT_EQ(kDeploymentHashKey.k0, 0x0706050403020100ULL);
        EXPECT_EQ(kDeploymentHashKey.k1, 0x0f0e0d0c0b0a0908ULL);
        EXPECT_EQ(CurrentKeyRows(p1.db(),
                                 "secret_type='siphash_id_key' AND status='current'"),
                  1);  // old auto key demoted, env key is the sole current
    }
    ::unsetenv("CORTRIX_SIPHASH_ID_KEY");
    std::remove(path.c_str());
}

TEST(IdHashKeyTest, NullDbFails) {
    ::unsetenv("CORTRIX_SIPHASH_ID_KEY");
    EXPECT_FALSE(LoadOrBootstrapHashKey(nullptr).ok());
}

TEST(IdHashKeyTest, MalformedEnvKeyFails) {
    ::setenv("CORTRIX_SIPHASH_ID_KEY", "not-hex", /*overwrite=*/1);
    auth::PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    EXPECT_FALSE(LoadOrBootstrapHashKey(pdb.db()).ok());
    ::unsetenv("CORTRIX_SIPHASH_ID_KEY");
}

// HashChildIdToBlockId uses the loaded global key and equals SipHash24 of the id
// bytes under that key (the documented contract), and is deterministic.
TEST(IdHashKeyTest, HashChildIdMatchesSipHashUnderKey) {
    SetDeploymentHashKeyForTesting(SipHashKey{0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL});
    const ChildId child = "01HQ8Z9K7M3N5P7R9T1V3W5X7Y";
    EXPECT_EQ(HashChildIdToBlockId(child),
              util::SipHash24(child.data(), child.size(), 0x0706050403020100ULL,
                              0x0f0e0d0c0b0a0908ULL));
    EXPECT_EQ(HashChildIdToBlockId(child), HashChildIdToBlockId(child));  // deterministic
}

// BlockId (uint64) must round-trip bit-exact through SQLite's signed int64 storage,
// including high-bit-set values (SipHash output) that are stored as negative int64.
TEST(IdSqliteIntTest, RoundTripIncludingHighBit) {
    const BlockId cases[] = {
        BlockId{0}, BlockId{1}, BlockId{0x7fffffffffffffffULL},
        BlockId{0x8000000000000000ULL}, BlockId{0xffffffffffffffffULL},
        BlockId{0xdeadbeefcafef00dULL},
    };
    for (BlockId v : cases) {
        EXPECT_EQ(FromSqliteInt(ToSqliteInt(v)), v);
    }
    // High-bit-set BlockIds really do become negative int64 (what SQLite stores).
    EXPECT_LT(ToSqliteInt(BlockId{0x8000000000000000ULL}), 0);
    EXPECT_GE(ToSqliteInt(BlockId{0x7fffffffffffffffULL}), 0);
}

}  // namespace
}  // namespace cortrix::id
