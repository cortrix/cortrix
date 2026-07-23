// F01 S1 — PHnsw class-framework tests.
//
// S1's DoD is "PHnsw class framework instantiable" on top of the vendored
// hnswlib fork. These tests exercise the live S1 surface (construct, add,
// search, exists, idempotent delete, stats, and the IVectorStore subset). The
// WAL/group-commit/snapshot durability paths land in S2–S6 and have their own
// tests there; the persistence stubs are only checked here for "doesn't crash /
// returns the documented S1 status".

#include "cortrix/store/phnsw.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/iindex.h"

namespace cortrix::store {
namespace {

namespace fs = std::filesystem;

// Deterministic helper (F01 §"test-data strategy" — fixed seed for repeatability).
std::vector<float> MakeVector(int dim, int index) {
    std::vector<float> v(static_cast<size_t>(dim));
    // Simple deterministic fill; distinct per index so neighbors are well-ordered.
    for (int i = 0; i < dim; ++i) {
        v[static_cast<size_t>(i)] = static_cast<float>((index * 31 + i) % 97) * 0.01f;
    }
    return v;
}

class PHnswTest : public ::testing::Test {
protected:
    void SetUp() override {
        unit_dir_ = (fs::temp_directory_path() /
                     ("cortrix_phnsw_s1_" + std::to_string(::testing::UnitTest::GetInstance()
                                                               ->current_test_info()
                                                               ->name()[0]) +
                      std::to_string(reinterpret_cast<uintptr_t>(this))))
                        .string();
        fs::remove_all(unit_dir_);
        config_.dim = kDim;
        config_.max_elements = 1000;
    }
    void TearDown() override { fs::remove_all(unit_dir_); }

    static constexpr int kDim = 16;
    std::string unit_dir_;
    PhnswConfig config_;
};

TEST_F(PHnswTest, ConstructsAndCreatesDataDir) {
    PHnsw index(unit_dir_, config_);
    EXPECT_TRUE(fs::exists(unit_dir_));
    auto stats = index.GetStats();
    EXPECT_EQ(stats.vector_count, 0u);
    EXPECT_EQ(stats.deleted_count, 0u);
    EXPECT_EQ(stats.dim, kDim);
}

TEST_F(PHnswTest, AddPointRejectsNull) {
    PHnsw index(unit_dir_, config_);
    Status s = index.AddPoint(nullptr, 1);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(PHnswTest, AddPointThenSearchFindsIt) {
    PHnsw index(unit_dir_, config_);
    auto v0 = MakeVector(kDim, 0);
    auto v1 = MakeVector(kDim, 50);
    ASSERT_TRUE(index.AddPoint(v0.data(), 100).ok());
    ASSERT_TRUE(index.AddPoint(v1.data(), 200).ok());

    auto results = index.Search(v0.data(), /*top_k=*/1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 100u);  // nearest to v0 is block_id 100 itself

    auto stats = index.GetStats();
    EXPECT_EQ(stats.vector_count, 2u);
}

TEST_F(PHnswTest, TwoPointSearchStaysWithinNeighborListBounds) {
    // A two-node graph produces one-entry adjacency lists. Under ASAN this
    // exercises the SSE look-ahead boundary that previously read neighbor[1]
    // while traversing a list whose only valid element was neighbor[0].
    PHnsw index(unit_dir_, config_);
    auto v0 = MakeVector(kDim, 0);
    auto v1 = MakeVector(kDim, 50);
    ASSERT_TRUE(index.AddPoint(v0.data(), 100).ok());
    ASSERT_TRUE(index.AddPoint(v1.data(), 200).ok());

    auto results = index.Search(v0.data(), /*top_k=*/2);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].first, 100u);
}

TEST_F(PHnswTest, AddPointsBatch) {
    PHnsw index(unit_dir_, config_);
    std::vector<std::vector<float>> store;
    std::vector<std::pair<const float*, uint64_t>> batch;
    for (int i = 0; i < 10; ++i) {
        store.push_back(MakeVector(kDim, i));
    }
    for (int i = 0; i < 10; ++i) {
        batch.emplace_back(store[static_cast<size_t>(i)].data(), static_cast<uint64_t>(i + 1));
    }
    ASSERT_TRUE(index.AddPoints(batch).ok());
    EXPECT_EQ(index.GetStats().vector_count, 10u);
}

TEST_F(PHnswTest, ExistsReflectsMembership) {
    PHnsw index(unit_dir_, config_);
    auto v = MakeVector(kDim, 7);
    EXPECT_FALSE(index.Exists(42));
    ASSERT_TRUE(index.AddPoint(v.data(), 42).ok());
    EXPECT_TRUE(index.Exists(42));
    EXPECT_FALSE(index.Exists(43));
}

TEST_F(PHnswTest, MarkDeleteIsIdempotent) {
    PHnsw index(unit_dir_, config_);
    auto v = MakeVector(kDim, 3);
    ASSERT_TRUE(index.AddPoint(v.data(), 5).ok());
    EXPECT_TRUE(index.Exists(5));

    // Delete present id.
    EXPECT_TRUE(index.MarkDelete(5).ok());
    EXPECT_FALSE(index.Exists(5));

    // Delete missing id — idempotent Ok (F25 Q6 + V5 #9).
    EXPECT_TRUE(index.MarkDelete(5).ok());
    EXPECT_TRUE(index.MarkDelete(99999).ok());
}

TEST_F(PHnswTest, AddPoint_AfterMarkDelete_SameBlockId_Reinserts) {
    // Regression for the store-C1 silent data-loss bug. F01 design § 5 sanctions a
    // re-write as "MarkDelete(id) then AddPoint(id)" with the SAME block_id. The
    // index is built with allow_replace_deleted_=true, under which hnswlib's 2-arg
    // addPoint THROWS on a marked-deleted label; that throw was swallowed by
    // ApplyEntryLocked's outer catch, so the re-inserted vector — already durable
    // in hnsw.wal — never re-entered the graph. After the fix the same id must come
    // back live, be searchable, and the vector must reflect the NEW value.
    PHnsw index(unit_dir_, config_);

    // A second, well-separated point so the graph is non-trivial and Search has a
    // real choice of nearest neighbor.
    auto other = MakeVector(kDim, 80);
    ASSERT_TRUE(index.AddPoint(other.data(), 200).ok());

    // Original vector for id 5, then delete id 5.
    auto v_old = MakeVector(kDim, 5);
    ASSERT_TRUE(index.AddPoint(v_old.data(), 5).ok());
    EXPECT_TRUE(index.Exists(5));
    ASSERT_TRUE(index.MarkDelete(5).ok());
    EXPECT_FALSE(index.Exists(5));
    EXPECT_EQ(index.GetStats().vector_count, 1u);   // only id 200 live
    EXPECT_EQ(index.GetStats().deleted_count, 1u);

    // Re-write id 5 with a DIFFERENT vector (the bug dropped this insert silently).
    auto v_new = MakeVector(kDim, 30);
    ASSERT_TRUE(index.AddPoint(v_new.data(), 5).ok());

    // id 5 is live again, the deletion was reclaimed, and both ids are searchable.
    EXPECT_TRUE(index.Exists(5));
    EXPECT_EQ(index.GetStats().vector_count, 2u);   // ids 5 and 200 live
    EXPECT_EQ(index.GetStats().deleted_count, 0u);  // resurrected, not double-counted

    // Querying with the NEW vector returns id 5 first → its stored vector was
    // overwritten with v_new (a stale v_old would not be nearest to v_new).
    auto hit_new = index.Search(v_new.data(), /*top_k=*/1);
    ASSERT_EQ(hit_new.size(), 1u);
    EXPECT_EQ(hit_new[0].first, 5u);
}

TEST_F(PHnswTest, AddPoint_AfterMarkDelete_SameBlockId_SurvivesRecovery) {
    // The re-write must also survive a reopen: the WAL holds DELETE(5) then
    // INSERT(5), and recovery replay must apply the same resurrection logic rather
    // than dropping the post-delete INSERT.
    auto v_new = MakeVector(kDim, 33);
    {
        PHnsw index(unit_dir_, config_);
        auto v_old = MakeVector(kDim, 5);
        ASSERT_TRUE(index.AddPoint(v_old.data(), 5).ok());
        ASSERT_TRUE(index.MarkDelete(5).ok());
        ASSERT_TRUE(index.AddPoint(v_new.data(), 5).ok());
        EXPECT_TRUE(index.Exists(5));
    }
    PHnsw reopened(unit_dir_, config_);
    EXPECT_TRUE(reopened.Exists(5));               // re-inserted id replayed live
    EXPECT_EQ(reopened.GetStats().vector_count, 1u);
    EXPECT_EQ(reopened.GetStats().deleted_count, 0u);
    auto hit = reopened.Search(v_new.data(), /*top_k=*/1);
    ASSERT_EQ(hit.size(), 1u);
    EXPECT_EQ(hit[0].first, 5u);
}

TEST_F(PHnswTest, SearchOnEmptyReturnsEmpty) {
    PHnsw index(unit_dir_, config_);
    auto q = MakeVector(kDim, 0);
    EXPECT_TRUE(index.Search(q.data(), 5).empty());
    EXPECT_TRUE(index.Search(nullptr, 5).empty());
    EXPECT_TRUE(index.Search(q.data(), 0).empty());
}

TEST_F(PHnswTest, MemoryFootprintGrowsWithVectors) {
    PHnsw index(unit_dir_, config_);
    const std::size_t empty_fp = index.GetMemoryFootprintBytes();
    auto v = MakeVector(kDim, 1);
    ASSERT_TRUE(index.AddPoint(v.data(), 1).ok());
    EXPECT_GT(index.GetMemoryFootprintBytes(), empty_fp);
}

TEST_F(PHnswTest, LifecycleOpsSucceedOnEmptyIndex) {
    PHnsw index(unit_dir_, config_);
    // S4: Recover (reload from disk), Snapshot (persist empty graph), and
    // Shutdown all succeed on a freshly opened empty index.
    EXPECT_TRUE(index.Recover().ok());
    EXPECT_TRUE(index.Snapshot().ok());
    EXPECT_TRUE(index.Shutdown().ok());
}

// PHnsw must satisfy both contracts it inherits: the rich IIndex SoT and the
// narrow IVectorStore subset that F25 WriteCoordinator holds.
TEST_F(PHnswTest, UsableThroughIIndexPointer) {
    PHnsw index(unit_dir_, config_);
    IIndex* as_index = &index;
    auto v = MakeVector(kDim, 2);
    ASSERT_TRUE(as_index->AddPoint(v.data(), 11).ok());
    EXPECT_TRUE(as_index->Exists(11));
    EXPECT_EQ(as_index->Search(v.data(), 1, /*ef_search=*/-1).size(), 1u);
}

TEST_F(PHnswTest, UsableThroughIVectorStorePointer) {
    PHnsw index(unit_dir_, config_);
    IVectorStore* as_store = &index;  // F25 holds this narrow surface
    auto v = MakeVector(kDim, 4);
    ASSERT_TRUE(as_store->AddPoint(v.data(), 77).ok());
    EXPECT_TRUE(as_store->Exists(77));                 // TraceContext-arity overload
    EXPECT_EQ(as_store->Search(v.data(), 1).size(), 1u);  // no-ef_search overload
    EXPECT_TRUE(as_store->MarkDelete(77).ok());
    EXPECT_FALSE(as_store->Exists(77));
}

// --- branch-coverage supplements (write/search/exists edge paths) ---------

TEST_F(PHnswTest, AddPointsEmptyBatchIsOk) {
    PHnsw index(unit_dir_, config_);
    // Empty points vector hits the early-return Ok branch (no submits).
    EXPECT_TRUE(index.AddPoints({}).ok());
    EXPECT_EQ(index.GetStats().vector_count, 0u);
}

TEST_F(PHnswTest, AddPointsRejectsNullInBatch) {
    PHnsw index(unit_dir_, config_);
    auto v = MakeVector(kDim, 1);
    std::vector<std::pair<const float*, uint64_t>> batch;
    batch.emplace_back(v.data(), 1u);
    batch.emplace_back(nullptr, 2u);  // null vector mid-batch → InvalidArgument
    Status s = index.AddPoints(batch);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(PHnswTest, SearchWithExplicitEfSearch) {
    // ef_search > 0 takes the exclusive-lock branch (per-query ef applied then
    // restored). Verify it returns the same nearest neighbor as the default path.
    PHnsw index(unit_dir_, config_);
    auto v0 = MakeVector(kDim, 0);
    auto v1 = MakeVector(kDim, 50);
    ASSERT_TRUE(index.AddPoint(v0.data(), 100).ok());
    ASSERT_TRUE(index.AddPoint(v1.data(), 200).ok());

    auto results = index.Search(v0.data(), /*top_k=*/1, /*ef_search=*/64);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 100u);
}

TEST_F(PHnswTest, SearchWithExplicitEfSearchOnEmptyIsEmpty) {
    // ef_search > 0 path on an empty graph: SearchLocked sees element_count==0.
    PHnsw index(unit_dir_, config_);
    auto q = MakeVector(kDim, 0);
    EXPECT_TRUE(index.Search(q.data(), 3, /*ef_search=*/32).empty());
    // Also guard the null-query / non-positive top_k branches on the ef path.
    EXPECT_TRUE(index.Search(nullptr, 3, /*ef_search=*/32).empty());
    EXPECT_TRUE(index.Search(q.data(), 0, /*ef_search=*/32).empty());
}

TEST_F(PHnswTest, SearchWhenAllDeletedReturnsEmpty) {
    // With every live vector deleted, SearchLocked's live==0 → actual_k==0 branch.
    PHnsw index(unit_dir_, config_);
    auto v = MakeVector(kDim, 9);
    ASSERT_TRUE(index.AddPoint(v.data(), 1).ok());
    ASSERT_TRUE(index.MarkDelete(1).ok());
    auto results = index.Search(v.data(), /*top_k=*/3);
    EXPECT_TRUE(results.empty());  // no live elements left
}

TEST_F(PHnswTest, ExistsFalseForDeletedAndAbsent) {
    PHnsw index(unit_dir_, config_);
    auto v = MakeVector(kDim, 5);
    ASSERT_TRUE(index.AddPoint(v.data(), 8).ok());
    EXPECT_TRUE(index.Exists(8));
    ASSERT_TRUE(index.MarkDelete(8).ok());
    EXPECT_FALSE(index.Exists(8));        // present in label map but marked deleted
    EXPECT_FALSE(index.Exists(123456));   // never inserted (label_lookup miss)
}

TEST_F(PHnswTest, SnapshotAndRecoverViaTraceContextOverloads) {
    // The IVectorStore TraceContext-arity overloads delegate to the no-ctx forms.
    PHnsw index(unit_dir_, config_);
    auto v = MakeVector(kDim, 2);
    ASSERT_TRUE(index.AddPoint(v.data(), 3).ok());
    IVectorStore* as_store = &index;
    EXPECT_TRUE(as_store->Snapshot(/*ctx=*/nullptr).ok());
    EXPECT_TRUE(as_store->Recover(/*ctx=*/nullptr).ok());
    EXPECT_TRUE(index.Exists(3));
}

TEST_F(PHnswTest, GetWalStatsReflectsAppends) {
    PHnsw index(unit_dir_, config_);
    EXPECT_EQ(index.GetWalStats().entry_count, 0u);
    auto v = MakeVector(kDim, 1);
    ASSERT_TRUE(index.AddPoint(v.data(), 1).ok());
    EXPECT_EQ(index.GetWalStats().entry_count, 1u);
    EXPECT_GE(index.GetWalStats().committed_lsn, 1u);
}

TEST_F(PHnswTest, ShutdownIsIdempotent) {
    // Shutdown stops the committer; a second call is a documented no-op (the
    // committer_ guard stays true but GroupCommitWriter::Shutdown tolerates it).
    PHnsw index(unit_dir_, config_);
    auto v = MakeVector(kDim, 1);
    ASSERT_TRUE(index.AddPoint(v.data(), 1).ok());
    EXPECT_TRUE(index.Shutdown().ok());
    EXPECT_TRUE(index.Shutdown().ok());  // second call also Ok
}

TEST_F(PHnswTest, RecoverAfterWritesIsConsistent) {
    // Recover() re-runs RecoverInternal under the write lock: with no snapshot it
    // reloads from an empty graph + replays the WAL, so membership is preserved.
    PHnsw index(unit_dir_, config_);
    auto v = MakeVector(kDim, 6);
    ASSERT_TRUE(index.AddPoint(v.data(), 60).ok());
    ASSERT_TRUE(index.AddPoint(MakeVector(kDim, 7).data(), 70).ok());
    ASSERT_TRUE(index.Recover().ok());
    EXPECT_TRUE(index.Exists(60));
    EXPECT_TRUE(index.Exists(70));
    EXPECT_EQ(index.GetStats().vector_count, 2u);
}

}  // namespace
}  // namespace cortrix::store
