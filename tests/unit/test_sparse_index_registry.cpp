// SparseIndexRegistry unit tests — covers the 0% line-coverage gap.
//
// Tests the registry's lazy-open lifecycle, path computation, in-memory fallback,
// cache/idempotency behavior, and namespace isolation. Real SpladeSparseRetriever
// is used under ":memory:" (no disk, no model required).
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "cortrix/retrieval/sparse_index_registry.h"
#include "cortrix/retrieval/sparse_retriever.h"

namespace cortrix::retrieval {
namespace {

// ---------------------------------------------------------------------------
// In-memory registry (data_dir == "") — the primary test vehicle.
// GetOrOpen returns a real SpladeSparseRetriever backed by SQLite ":memory:".
// ---------------------------------------------------------------------------

TEST(SparseIndexRegistryTest, GetOrOpen_EmptyDataDir_ReturnsNonNull) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* r = reg.GetOrOpen("ns1");
    ASSERT_NE(r, nullptr);
}

// The returned retriever is open and available (IsAvailable = true after Open()).
TEST(SparseIndexRegistryTest, GetOrOpen_EmptyDataDir_RetrieverIsAvailable) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* r = reg.GetOrOpen("ns1");
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->IsAvailable());
}

// Second call for the same ns_id returns the SAME cached pointer.
TEST(SparseIndexRegistryTest, GetOrOpen_SameNsSamePointer) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* r1 = reg.GetOrOpen("ns-alpha");
    ISparseRetriever* r2 = reg.GetOrOpen("ns-alpha");
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(r1, r2);
}

// Different ns_ids get different retriever instances.
TEST(SparseIndexRegistryTest, GetOrOpen_DifferentNsDifferentPointers) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* ra = reg.GetOrOpen("ns-a");
    ISparseRetriever* rb = reg.GetOrOpen("ns-b");
    ASSERT_NE(ra, nullptr);
    ASSERT_NE(rb, nullptr);
    EXPECT_NE(ra, rb);
}

// Three distinct namespaces all get non-null, distinct retrievers.
TEST(SparseIndexRegistryTest, GetOrOpen_MultipleNamespaces_AllNonNull) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* r1 = reg.GetOrOpen("ns-1");
    ISparseRetriever* r2 = reg.GetOrOpen("ns-2");
    ISparseRetriever* r3 = reg.GetOrOpen("ns-3");
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    ASSERT_NE(r3, nullptr);
    EXPECT_NE(r1, r2);
    EXPECT_NE(r1, r3);
    EXPECT_NE(r2, r3);
}

// The registry is callable through multiple sequential round-trips. Previously
// cached entries survive across GetOrOpen calls for other namespaces.
TEST(SparseIndexRegistryTest, GetOrOpen_CacheStableAcrossOtherOpens) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* first = reg.GetOrOpen("ns-stable");
    reg.GetOrOpen("ns-other1");
    reg.GetOrOpen("ns-other2");
    ISparseRetriever* second = reg.GetOrOpen("ns-stable");
    EXPECT_EQ(first, second);
}

// Add → Search round-trip through the registry. Verifies the retriever returned
// by GetOrOpen is functional for write + read (end-to-end through the cached
// SpladeSparseRetriever). Namespace isolation: ns-A writes don't appear in ns-B.
TEST(SparseIndexRegistryTest, GetOrOpen_AddThenSearch_WorksForInMemory) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* r = reg.GetOrOpen("ns-rw");
    ASSERT_NE(r, nullptr);

    SparseVector vec;
    vec.terms[1] = 0.9f;
    vec.terms[2] = 0.4f;
    ASSERT_TRUE(r->Add("ns-rw", "child-001", vec).ok());

    SparseVector query;
    query.terms[1] = 1.0f;
    auto hits = r->Search(query, "ns-rw", 10);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].child_id, "child-001");
}

TEST(SparseIndexRegistryTest, GetOrOpen_NamespaceIsolation_NoLeakBetweenNs) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* ra = reg.GetOrOpen("ns-iso-a");
    ISparseRetriever* rb = reg.GetOrOpen("ns-iso-b");
    ASSERT_NE(ra, nullptr);
    ASSERT_NE(rb, nullptr);

    SparseVector vec;
    vec.terms[42] = 1.0f;
    ASSERT_TRUE(ra->Add("ns-iso-a", "child-in-a", vec).ok());

    SparseVector query;
    query.terms[42] = 1.0f;
    // ns-iso-b retriever must not see ns-iso-a's writes.
    auto hits_b = rb->Search(query, "ns-iso-b", 10);
    EXPECT_TRUE(hits_b.empty());
    // ns-iso-a retriever finds its own entry.
    auto hits_a = ra->Search(query, "ns-iso-a", 10);
    ASSERT_FALSE(hits_a.empty());
    EXPECT_EQ(hits_a[0].child_id, "child-in-a");
}

// The in-memory path (:memory:) is always used when data_dir == "".
// A namespace with a path separator in the name is still valid in-memory.
TEST(SparseIndexRegistryTest, GetOrOpen_SpecialCharNsId_InMemory) {
    SparseIndexRegistry reg("", SpladeConfig{});
    // A reasonable ns_id that might contain non-trivial characters.
    ISparseRetriever* r = reg.GetOrOpen("tenant/ns_special-01");
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->IsAvailable());
}

// Repeated idempotent Add for the same child_id replaces postings.
TEST(SparseIndexRegistryTest, GetOrOpen_ReAddReplaces_ViaRegistry) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* r = reg.GetOrOpen("ns-replace");
    ASSERT_NE(r, nullptr);

    SparseVector v1;
    v1.terms[10] = 0.8f;
    ASSERT_TRUE(r->Add("ns-replace", "ch", v1).ok());

    SparseVector v2;
    v2.terms[20] = 0.6f;
    ASSERT_TRUE(r->Add("ns-replace", "ch", v2).ok());

    SparseVector q1;
    q1.terms[10] = 1.0f;
    EXPECT_TRUE(r->Search(q1, "ns-replace", 10).empty());  // old term gone

    SparseVector q2;
    q2.terms[20] = 1.0f;
    auto hits = r->Search(q2, "ns-replace", 10);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].child_id, "ch");
}

// Empty SparseVector Add removes the child's postings (dead-chunk path).
TEST(SparseIndexRegistryTest, GetOrOpen_AddEmpty_RemovesPostings) {
    SparseIndexRegistry reg("", SpladeConfig{});
    ISparseRetriever* r = reg.GetOrOpen("ns-dead");
    ASSERT_NE(r, nullptr);

    SparseVector vec;
    vec.terms[5] = 0.7f;
    ASSERT_TRUE(r->Add("ns-dead", "dying-child", vec).ok());

    SparseVector empty;
    ASSERT_TRUE(r->Add("ns-dead", "dying-child", empty).ok());  // dead-chunk

    SparseVector query;
    query.terms[5] = 1.0f;
    EXPECT_TRUE(r->Search(query, "ns-dead", 10).empty());
}

// ---------------------------------------------------------------------------
// File-backed registry (data_dir set to a temp directory).
// Tests that GetOrOpen creates the per-NS subdirectory and returns a working
// retriever. Skipped on platforms where tmp dir creation fails.
// ---------------------------------------------------------------------------

TEST(SparseIndexRegistryFileTest, GetOrOpen_WithDataDir_CreatesSubdir) {
    std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "sparse_reg_test_XXXXXX";
    // Create a unique temp dir for this test.
    std::filesystem::create_directories(tmp);

    {
        SparseIndexRegistry reg(tmp.string(), SpladeConfig{});
        ISparseRetriever* r = reg.GetOrOpen("test-ns");
        ASSERT_NE(r, nullptr);
        EXPECT_TRUE(r->IsAvailable());

        // The per-NS directory <data_dir>/test-ns/ must exist now.
        EXPECT_TRUE(
            std::filesystem::exists(tmp / "test-ns"));
    }

    // Cleanup.
    std::filesystem::remove_all(tmp);
}

// A second GetOrOpen for the same NS (file-backed) returns the cached pointer
// without reopening the SQLite file (idempotent).
TEST(SparseIndexRegistryFileTest, GetOrOpen_WithDataDir_CachesRetriever) {
    std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "sparse_reg_cache_test";
    std::filesystem::create_directories(tmp);

    {
        SparseIndexRegistry reg(tmp.string(), SpladeConfig{});
        ISparseRetriever* r1 = reg.GetOrOpen("cached-ns");
        ISparseRetriever* r2 = reg.GetOrOpen("cached-ns");
        ASSERT_NE(r1, nullptr);
        EXPECT_EQ(r1, r2);
    }

    std::filesystem::remove_all(tmp);
}

// Verify that config fields (default_top_k) are threaded through to the retriever.
// We can only observe this indirectly via SpladeConfig default_top_k with a search.
TEST(SparseIndexRegistryTest, GetOrOpen_ConfigPassthrough_DefaultTopK) {
    SpladeConfig cfg;
    cfg.default_top_k = 5;
    SparseIndexRegistry reg("", cfg);

    ISparseRetriever* r = reg.GetOrOpen("ns-cfg");
    ASSERT_NE(r, nullptr);

    // Add 10 children and query with top_k=-1 (use default from config = 5).
    for (int i = 0; i < 10; ++i) {
        SparseVector vec;
        vec.terms[1] = static_cast<float>(i + 1) * 0.1f;
        r->Add("ns-cfg", "child-" + std::to_string(i), vec);
    }
    SparseVector query;
    query.terms[1] = 1.0f;
    auto hits = r->Search(query, "ns-cfg", -1);  // top_k <= 0 uses config.default_top_k
    EXPECT_LE(static_cast<int>(hits.size()), 5);
}

}  // namespace
}  // namespace cortrix::retrieval
