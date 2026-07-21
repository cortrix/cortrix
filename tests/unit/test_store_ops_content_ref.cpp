// test_store_ops_content_ref.cpp — BREADTH matrices for DefaultContentRefStore
// IncrementRef / DecrementRef / GetRefCount / GetGCCandidates over refcount,
// below-zero, and unknown-triple matrices. Complements test_content_ref_store.cpp
// (named one-off cases) with parameterized sweeps against a real in-memory
// catalog.db.
//
// Deterministic: CatalogDb(":memory:"), no network/LLM. gtest suite/fixture
// names are GLOBAL strings — every name here is prefixed ContentRefOps* to stay
// globally unique (no clash with ContentRefStoreTest etc.).
// APIs verified against include/cortrix/catalog/{i_content_ref_store,
// default_content_ref_store,catalog_db}.h:
//   Status      IncrementRef(file_hash, ns_id, doc_id);
//   Result<int> DecrementRef(file_hash, ns_id, doc_id);   // remaining count
//   Result<int> GetRefCount(file_hash) const;             // 0 if unknown
//   Result<std::vector<std::string>> GetGCCandidates(before_ms) const;

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/default_content_ref_store.h"

namespace cortrix::catalog {
namespace {

bool HasCx(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;
}

// Seed a file_locations row so the ref_count aggregate is tracked (mirrors the
// helper in test_content_ref_store.cpp; FK parents seeded in the fixture).
void SeedFileLocation(sqlite3* db, const std::string& file_hash,
                      const std::string& status = "active",
                      std::optional<int64_t> deleted_at = std::nullopt) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO file_locations (file_hash, unit_id, ns_id, tenant_id, "
        "ref_count, first_seen_at, status, deleted_at, created_at) "
        "VALUES (?, 'u1', 'ns1', 't1', 0, 0, ?, ?, 0)";
    ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, file_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    if (deleted_at.has_value()) sqlite3_bind_int64(stmt, 3, *deleted_at);
    else sqlite3_bind_null(stmt, 3);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

class ContentRefOpsBase : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        store_ = std::make_unique<DefaultContentRefStore>(catalog_.db());
    }
    int RefCount(const std::string& fh) {
        auto c = store_->GetRefCount(fh);
        EXPECT_TRUE(c.ok()) << c.status().message();
        return c.ok() ? c.value() : -999;
    }
    CatalogDb catalog_;
    std::unique_ptr<DefaultContentRefStore> store_;
};

// =============================================================================
// MATRIX 1 — N distinct triples on one file_hash → ref_count == N, then
// decrement them all the way back to 0. Parameterized over N.
// =============================================================================
class ContentRefOpsFanout : public ContentRefOpsBase,
                            public ::testing::WithParamInterface<int> {};

TEST_P(ContentRefOpsFanout, IncrementNDistinctTriplesCountsN) {
    const int n = GetParam();
    SeedFileLocation(catalog_.db(), "fh");
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(store_->IncrementRef("fh", "ns" + std::to_string(i),
                                         "doc" + std::to_string(i)).ok());
    }
    EXPECT_EQ(RefCount("fh"), n);
}

TEST_P(ContentRefOpsFanout, DecrementAllTriplesReachesZero) {
    const int n = GetParam();
    SeedFileLocation(catalog_.db(), "fh");
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(store_->IncrementRef("fh", "ns" + std::to_string(i),
                                         "doc" + std::to_string(i)).ok());
    }
    // Decrement in order; each returns the remaining count.
    for (int i = 0; i < n; ++i) {
        auto r = store_->DecrementRef("fh", "ns" + std::to_string(i),
                                      "doc" + std::to_string(i));
        ASSERT_TRUE(r.ok()) << r.status().message();
        EXPECT_EQ(r.value(), n - 1 - i) << "after decrement " << i;
    }
    EXPECT_EQ(RefCount("fh"), 0);
}

INSTANTIATE_TEST_SUITE_P(Fanout, ContentRefOpsFanout,
                         ::testing::Values(1, 2, 3, 4, 5, 6, 8, 10, 16, 24, 32));

// =============================================================================
// MATRIX 2 — idempotency: incrementing the SAME triple K times still counts 1.
// =============================================================================
class ContentRefOpsIdempotent : public ContentRefOpsBase,
                                public ::testing::WithParamInterface<int> {};

TEST_P(ContentRefOpsIdempotent, RepeatedSameTripleCountsOnce) {
    const int k = GetParam();
    SeedFileLocation(catalog_.db(), "fh");
    for (int i = 0; i < k; ++i) {
        ASSERT_TRUE(store_->IncrementRef("fh", "ns1", "doc1").ok());
    }
    EXPECT_EQ(RefCount("fh"), 1) << "re-referencing same triple must not double count";
    // One decrement takes it to 0 regardless of how many increments ran.
    auto r = store_->DecrementRef("fh", "ns1", "doc1");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 0);
}

INSTANTIATE_TEST_SUITE_P(Repeat, ContentRefOpsIdempotent,
                         ::testing::Values(1, 2, 3, 4, 5, 7, 10));

// =============================================================================
// MATRIX 3 — reactivation cycle: inc/dec/inc on the same triple. Parameterized
// over how many inc/dec cycles to run; count is always 1 at end of an inc.
// =============================================================================
class ContentRefOpsCycle : public ContentRefOpsBase,
                           public ::testing::WithParamInterface<int> {};

TEST_P(ContentRefOpsCycle, IncDecReactivateCycles) {
    const int cycles = GetParam();
    SeedFileLocation(catalog_.db(), "fh");
    for (int c = 0; c < cycles; ++c) {
        ASSERT_TRUE(store_->IncrementRef("fh", "ns1", "doc1").ok());
        EXPECT_EQ(RefCount("fh"), 1) << "after inc cycle " << c;
        auto r = store_->DecrementRef("fh", "ns1", "doc1");
        ASSERT_TRUE(r.ok());
        EXPECT_EQ(r.value(), 0) << "after dec cycle " << c;
    }
    EXPECT_EQ(RefCount("fh"), 0);
}

INSTANTIATE_TEST_SUITE_P(Cycles, ContentRefOpsCycle,
                         ::testing::Values(1, 2, 3, 4, 5, 8, 10));

// =============================================================================
// MATRIX 4 — below-zero guard: an active content_refs row whose aggregate is
// already 0 must refuse to go negative (CX_ERR_REF_COUNT_NEGATIVE).
// Parameterized over a few (ns,doc) labels to exercise the same guard path.
// =============================================================================
struct BelowZeroCase { std::string name; std::string ns; std::string doc; };

class ContentRefOpsBelowZero : public ContentRefOpsBase,
                               public ::testing::WithParamInterface<BelowZeroCase> {};

TEST_P(ContentRefOpsBelowZero, DecrementBelowZeroIsNegativeError) {
    const auto& p = GetParam();
    SeedFileLocation(catalog_.db(), "fh");  // aggregate ref_count = 0
    // Inject an active content_refs row WITHOUT bumping the aggregate, forcing
    // the inconsistent state the guard must reject.
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO content_refs (file_hash, ns_id, doc_id, target_unit_id, "
        "status, created_at) VALUES ('fh', ?, ?, '', 'active', 0)";
    ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(), sql, -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, p.ns.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, p.doc.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);

    auto r = store_->DecrementRef("fh", p.ns, p.doc);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(HasCx(r.status(), "CX_ERR_REF_COUNT_NEGATIVE")) << r.status().message();
}

INSTANTIATE_TEST_SUITE_P(
    Guard, ContentRefOpsBelowZero,
    ::testing::Values(
        BelowZeroCase{"plain", "ns1", "docX"},
        BelowZeroCase{"other_ns", "nsZ", "docY"},
        BelowZeroCase{"long_doc", "ns1", "01JCRDOC00000000000000000001"}),
    [](const ::testing::TestParamInfo<BelowZeroCase>& i) { return i.param.name; });

// =============================================================================
// MATRIX 5 — unknown-triple / unknown-file behaviors.
// =============================================================================

// GetRefCount on a never-seen file_hash is a clean 0.
class ContentRefOpsUnknownFile : public ContentRefOpsBase,
                                 public ::testing::WithParamInterface<std::string> {};

TEST_P(ContentRefOpsUnknownFile, GetRefCountUnknownIsZero) {
    EXPECT_EQ(RefCount(GetParam()), 0);
}

INSTANTIATE_TEST_SUITE_P(
    Unknown, ContentRefOpsUnknownFile,
    ::testing::Values("never", "0xdeadbeef", "fh-not-seeded",
                      "01JCRUNKNOWNFILE000000000001"),
    [](const ::testing::TestParamInfo<std::string>& i) {
        std::string s = i.param;
        for (auto& c : s) if (!std::isalnum((unsigned char)c)) c = '_';
        return s;
    });

// Decrement of a triple with no active content_refs row is a no-op (count
// unchanged, call succeeds). Parameterized over the starting ref_count.
class ContentRefOpsUnknownTriple : public ContentRefOpsBase,
                                   public ::testing::WithParamInterface<int> {};

TEST_P(ContentRefOpsUnknownTriple, DecrementUnknownTripleIsNoOp) {
    const int seeded_refs = GetParam();
    SeedFileLocation(catalog_.db(), "fh");
    for (int i = 0; i < seeded_refs; ++i) {
        ASSERT_TRUE(store_->IncrementRef("fh", "ns" + std::to_string(i),
                                         "doc" + std::to_string(i)).ok());
    }
    auto r = store_->DecrementRef("fh", "ns_never", "doc_never");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), seeded_refs) << "no active row → aggregate unchanged";
    EXPECT_EQ(RefCount("fh"), seeded_refs);
}

INSTANTIATE_TEST_SUITE_P(StartCount, ContentRefOpsUnknownTriple,
                         ::testing::Values(0, 1, 2, 3, 5, 8));

// Increment without a file_locations row still records the ref row; aggregate
// stays 0 (untracked). Parameterized over how many distinct orphan triples.
class ContentRefOpsOrphan : public ContentRefOpsBase,
                            public ::testing::WithParamInterface<int> {};

TEST_P(ContentRefOpsOrphan, IncrementWithoutAggregateRowTracksRefButCountZero) {
    const int n = GetParam();
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(store_->IncrementRef("orphan", "ns" + std::to_string(i),
                                         "doc" + std::to_string(i)).ok());
    }
    EXPECT_EQ(RefCount("orphan"), 0) << "no file_locations row → aggregate untracked";
    // Decrement of an orphan triple succeeds with count 0 (no aggregate).
    if (n > 0) {
        auto r = store_->DecrementRef("orphan", "ns0", "doc0");
        ASSERT_TRUE(r.ok()) << r.status().message();
        EXPECT_EQ(r.value(), 0);
    }
}

INSTANTIATE_TEST_SUITE_P(Orphans, ContentRefOpsOrphan,
                         ::testing::Values(0, 1, 2, 3, 4, 6, 8));

// =============================================================================
// MATRIX 6 — GC candidate cutoff matrix. Seed deleted files at deleted_at
// {100,500,900} (ref 0) + one active file (ref 1, never a candidate). Sweep at
// various cutoffs; only deleted_at <= cutoff with ref 0 qualify.
// =============================================================================
struct GcCutoffCase {
    std::string name;
    int64_t cutoff_ms;
    std::vector<std::string> expect;  // expected candidates (any order)
};

class ContentRefOpsGcCutoff : public ContentRefOpsBase,
                              public ::testing::WithParamInterface<GcCutoffCase> {};

TEST_P(ContentRefOpsGcCutoff, CandidatesAreZeroRefDeletedBeforeCutoff) {
    const auto& p = GetParam();
    SeedFileLocation(catalog_.db(), "fh_old",  "deleted", 100);
    SeedFileLocation(catalog_.db(), "fh_mid",  "deleted", 500);
    SeedFileLocation(catalog_.db(), "fh_new",  "deleted", 900);
    SeedFileLocation(catalog_.db(), "fh_null", "deleted");           // deleted_at NULL → excluded
    SeedFileLocation(catalog_.db(), "fh_live", "active");
    ASSERT_TRUE(store_->IncrementRef("fh_live", "ns1", "d").ok());   // ref 1

    auto cand = store_->GetGCCandidates(p.cutoff_ms);
    ASSERT_TRUE(cand.ok()) << cand.status().message();
    std::vector<std::string> got = cand.value();
    std::sort(got.begin(), got.end());
    std::vector<std::string> want = p.expect;
    std::sort(want.begin(), want.end());
    EXPECT_EQ(got, want) << p.name;
}

INSTANTIATE_TEST_SUITE_P(
    Cutoffs, ContentRefOpsGcCutoff,
    // NOTE: the GC query is STRICT — `deleted_at < cutoff` (see
    // default_content_ref_store.cpp) — so a file deleted exactly AT the cutoff
    // does NOT qualify.
    ::testing::Values(
        GcCutoffCase{"before_all", 0, {}},
        GcCutoffCase{"at_100_strict_excludes", 100, {}},
        GcCutoffCase{"between_100_500", 300, {"fh_old"}},
        GcCutoffCase{"at_500_strict_excludes_mid", 500, {"fh_old"}},
        GcCutoffCase{"between_500_900", 700, {"fh_old", "fh_mid"}},
        GcCutoffCase{"at_900_strict_excludes_new", 900, {"fh_old", "fh_mid"}},
        GcCutoffCase{"after_all", 100000, {"fh_old", "fh_mid", "fh_new"}}),
    [](const ::testing::TestParamInfo<GcCutoffCase>& i) { return i.param.name; });

// A file that was deleted (ref 0) but then re-referenced (ref back up) must drop
// out of the candidate set. Parameterized over the cutoff (always covers t=100).
class ContentRefOpsGcReref : public ContentRefOpsBase,
                             public ::testing::WithParamInterface<int64_t> {};

TEST_P(ContentRefOpsGcReref, ReReferencedFileIsNotCandidate) {
    SeedFileLocation(catalog_.db(), "fh", "deleted", 100);
    // Re-reference: ref_count goes back to 1 (this does not by itself clear the
    // 'deleted' status / deleted_at, but a live ref must exclude it from GC).
    ASSERT_TRUE(store_->IncrementRef("fh", "ns1", "doc1").ok());
    EXPECT_EQ(RefCount("fh"), 1);

    auto cand = store_->GetGCCandidates(GetParam());
    ASSERT_TRUE(cand.ok()) << cand.status().message();
    for (const auto& c : cand.value()) {
        EXPECT_NE(c, "fh") << "file with a live ref must not be a GC candidate";
    }
}

INSTANTIATE_TEST_SUITE_P(Cutoff, ContentRefOpsGcReref,
                         ::testing::Values<int64_t>(100, 500, 100000));

}  // namespace
}  // namespace cortrix::catalog
