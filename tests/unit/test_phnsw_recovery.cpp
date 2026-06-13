// F01 S4 — snapshot + crash recovery tests (design § 4.3/4.4 / § 6 S4).
//
// Exercises PHnsw's durable lifecycle end to end: write -> reopen replays the
// WAL; Snapshot() captures the graph + truncates the WAL; reopen after a
// snapshot loads it (and replays only post-snapshot WAL records via LSN
// filtering); corrupt WAL tail / corrupt snapshot fall back gracefully. Also
// covers SnapshotManager save/load/clean directly.

#include "cortrix/store/phnsw.h"
#include "cortrix/store/phnsw/snapshot_manager.h"

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <hnswlib/hnswlib.h>

namespace cortrix::store {
namespace {

namespace fs = std::filesystem;

std::vector<float> MakeVec(int dim, int seed) {
    std::vector<float> v(static_cast<size_t>(dim));
    for (int i = 0; i < dim; ++i) {
        v[static_cast<size_t>(i)] = static_cast<float>((seed * 17 + i) % 199) * 0.021f;
    }
    return v;
}

class PHnswRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = (fs::temp_directory_path() /
                ("cortrix_phnsw_rec_" + std::to_string(reinterpret_cast<uintptr_t>(this))))
                   .string();
        fs::remove_all(dir_);
        config_.dim = kDim;
        config_.max_elements = 5000;
        config_.snapshot_keep_count = 2;
    }
    void TearDown() override { fs::remove_all(dir_); }

    static constexpr int kDim = 8;
    std::string dir_;
    PhnswConfig config_;
};

// --- end-to-end recovery via PHnsw ---------------------------------------

TEST_F(PHnswRecoveryTest, Recovery_WalOnly) {
    // Write through one PHnsw, then reopen: with no snapshot, recovery replays
    // the whole WAL.
    {
        PHnsw idx(dir_, config_);
        for (int i = 0; i < 20; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
    }
    PHnsw reopened(dir_, config_);
    EXPECT_EQ(reopened.GetStats().vector_count, 20u);
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(reopened.Exists(static_cast<uint64_t>(i + 1))) << "missing id " << (i + 1);
    }
}

TEST_F(PHnswRecoveryTest, Recovery_SnapshotOnly) {
    // Write, snapshot (truncates WAL), reopen: recovery loads from the snapshot
    // alone (WAL is empty).
    {
        PHnsw idx(dir_, config_);
        for (int i = 0; i < 15; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
        ASSERT_TRUE(idx.Snapshot().ok());
        EXPECT_EQ(idx.GetWalStats().entry_count, 0u);  // WAL truncated post-snapshot
    }
    PHnsw reopened(dir_, config_);
    EXPECT_EQ(reopened.GetStats().vector_count, 15u);
    EXPECT_TRUE(reopened.Exists(1));
    EXPECT_TRUE(reopened.Exists(15));
}

TEST_F(PHnswRecoveryTest, Recovery_SnapshotPlusWal) {
    // Snapshot a prefix, then write more (post-snapshot WAL), reopen: recovery
    // = snapshot + replay of the post-snapshot WAL records.
    {
        PHnsw idx(dir_, config_);
        for (int i = 0; i < 10; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
        ASSERT_TRUE(idx.Snapshot().ok());
        for (int i = 10; i < 25; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
        EXPECT_EQ(idx.GetWalStats().entry_count, 15u);  // only post-snapshot writes
    }
    PHnsw reopened(dir_, config_);
    EXPECT_EQ(reopened.GetStats().vector_count, 25u);
    EXPECT_TRUE(reopened.Exists(1));    // from snapshot
    EXPECT_TRUE(reopened.Exists(25));   // from replayed WAL
}

TEST_F(PHnswRecoveryTest, Recovery_DeletePersists) {
    {
        PHnsw idx(dir_, config_);
        for (int i = 0; i < 5; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
        ASSERT_TRUE(idx.MarkDelete(3).ok());
    }
    PHnsw reopened(dir_, config_);
    EXPECT_TRUE(reopened.Exists(1));
    EXPECT_FALSE(reopened.Exists(3));  // delete replayed
    EXPECT_EQ(reopened.GetStats().vector_count, 4u);
}

TEST_F(PHnswRecoveryTest, Recovery_SnapshotThenDeleteThenReopen) {
    {
        PHnsw idx(dir_, config_);
        for (int i = 0; i < 8; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
        ASSERT_TRUE(idx.Snapshot().ok());
        ASSERT_TRUE(idx.MarkDelete(2).ok());  // post-snapshot delete
    }
    PHnsw reopened(dir_, config_);
    EXPECT_FALSE(reopened.Exists(2));
    EXPECT_TRUE(reopened.Exists(1));
}

TEST_F(PHnswRecoveryTest, Recovery_SearchConsistentAfterReopen) {
    auto q = MakeVec(kDim, 3);
    {
        PHnsw idx(dir_, config_);
        for (int i = 0; i < 30; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
    }
    PHnsw reopened(dir_, config_);
    auto results = reopened.Search(q.data(), 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 4u);  // id 4 == MakeVec seed 3 (i+1)
}

TEST_F(PHnswRecoveryTest, Recovery_WalCorruptedTail_Truncated) {
    {
        PHnsw idx(dir_, config_);
        for (int i = 0; i < 10; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
    }
    // Corrupt the last byte of hnsw.wal (the tail record).
    const std::string wal = (fs::path(dir_) / "hnsw.wal").string();
    {
        int fd = ::open(wal.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        off_t end = ::lseek(fd, 0, SEEK_END);
        uint8_t b = 0;
        ASSERT_EQ(::pread(fd, &b, 1, end - 1), 1);
        b ^= 0xFF;
        ASSERT_EQ(::pwrite(fd, &b, 1, end - 1), 1);
        ::close(fd);
    }
    // Reopen: the corrupt tail record is dropped; the rest recover.
    PHnsw reopened(dir_, config_);
    EXPECT_EQ(reopened.GetStats().vector_count, 9u);
}

TEST_F(PHnswRecoveryTest, Recovery_WalCorruptedHeader_RecoverReturnsErrorNotCrash) {
    // Phase-QA C1-1 regression: a corrupt WAL *header* (bad magic / CRC, dim-independent,
    // hits at dim=1024 too) makes WalWriter::Open fail -> PHnsw ctor leaves wal_=null + NOT
    // READY. The factory Open path then calls Recover() unconditionally; before the
    // RecoverInternal null guard this null-deref'd (SIGSEGV). It must now return a clean
    // error (CX_ERR_PHNSW_NOT_READY), not crash. Prior recovery tests only covered
    // tail/snapshot corruption, not header corruption -> this test fills that blind spot.
    {
        PHnsw idx(dir_, config_);
        for (int i = 0; i < 5; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
    }
    // Corrupt the WAL header magic (offset 0) — header validation is dim-independent.
    const std::string wal = (fs::path(dir_) / "hnsw.wal").string();
    {
        int fd = ::open(wal.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        uint8_t b = 0;
        ASSERT_EQ(::pread(fd, &b, 1, 0), 1);
        b ^= 0xFF;
        ASSERT_EQ(::pwrite(fd, &b, 1, 0), 1);
        ::close(fd);
    }
    // ctor on corrupt-header WAL → NOT READY (wal_=null), must not crash.
    PHnsw reopened(dir_, config_);
    // The C1-1 crash path: Recover() on the not-ready object must return a clean
    // error, not SIGSEGV (RecoverInternal null guard).
    Status st = reopened.Recover();
    EXPECT_FALSE(st.ok());
}

// --- SnapshotManager direct tests ----------------------------------------

TEST_F(PHnswRecoveryTest, SnapshotManager_SaveAndLoad) {
    fs::create_directories(dir_);
    hnswlib::L2Space space(kDim);
    auto index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        &space, 1000, 16, 200, 100, true);
    for (int i = 0; i < 50; ++i) {
        auto v = MakeVec(kDim, i);
        index->addPoint(v.data(), static_cast<hnswlib::labeltype>(i + 1));
    }

    SnapshotManager mgr(dir_, 2);
    ASSERT_TRUE(mgr.Save(index.get(), /*lsn=*/50, /*dim=*/kDim, /*M=*/16).ok());
    EXPECT_EQ(mgr.LatestSnapshotLsn(), 50u);

    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 0;
    ASSERT_TRUE(mgr.Load(&space, 1000, &loaded, &lsn).ok());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(lsn, 50u);
    EXPECT_EQ(loaded->cur_element_count, 50u);
}

TEST_F(PHnswRecoveryTest, SnapshotManager_LoadNoneWhenEmpty) {
    fs::create_directories(dir_);
    hnswlib::L2Space space(kDim);
    SnapshotManager mgr(dir_, 2);
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 0;
    ASSERT_TRUE(mgr.Load(&space, 1000, &loaded, &lsn).ok());
    EXPECT_EQ(loaded, nullptr);  // no snapshot -> caller starts empty
    EXPECT_EQ(lsn, 0u);
}

TEST_F(PHnswRecoveryTest, SnapshotManager_CleanOld_KeepsTwo) {
    fs::create_directories(dir_);
    hnswlib::L2Space space(kDim);
    auto index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        &space, 1000, 16, 200, 100, true);
    auto v = MakeVec(kDim, 1);
    index->addPoint(v.data(), 1);

    SnapshotManager mgr(dir_, 2);
    // Four saves; each save also prunes to keep_count=2. Distinct timestamps via
    // a tiny sleep is overkill — Save stamps by ms; loop a few extra to be safe.
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(mgr.Save(index.get(), static_cast<uint64_t>(i + 1), /*dim=*/kDim, /*M=*/16).ok());
    }
    // Count .index files remaining.
    int index_files = 0;
    for (const auto& e : fs::directory_iterator(dir_)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("hnsw_", 0) == 0 && n.size() >= 6 &&
            n.compare(n.size() - 6, 6, ".index") == 0) {
            ++index_files;
        }
    }
    EXPECT_LE(index_files, 2);  // pruned to keep_count
}

TEST_F(PHnswRecoveryTest, SnapshotManager_CorruptSnapshot_Detected) {
    fs::create_directories(dir_);
    hnswlib::L2Space space(kDim);
    auto index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        &space, 1000, 16, 200, 100, true);
    auto v = MakeVec(kDim, 1);
    index->addPoint(v.data(), 1);

    SnapshotManager mgr(dir_, 2);
    ASSERT_TRUE(mgr.Save(index.get(), 1, /*dim=*/kDim, /*M=*/16).ok());

    // Corrupt the single .index file body so its CRC no longer matches the meta.
    for (const auto& e : fs::directory_iterator(dir_)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("hnsw_", 0) == 0 && n.size() >= 6 &&
            n.compare(n.size() - 6, 6, ".index") == 0) {
            int fd = ::open(e.path().c_str(), O_RDWR);
            ASSERT_GE(fd, 0);
            uint8_t b = 0;
            ASSERT_EQ(::pread(fd, &b, 1, 0), 1);
            b ^= 0xFF;
            ASSERT_EQ(::pwrite(fd, &b, 1, 0), 1);
            ::close(fd);
        }
    }
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 0;
    Status s = mgr.Load(&space, 1000, &loaded, &lsn);
    // Only snapshot is corrupt -> CX_ERR_PHNSW_SNAPSHOT_CORRUPTED.
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PHNSW_SNAPSHOT_CORRUPTED"), std::string::npos);
}

// --- SnapshotManager branch-coverage supplements --------------------------

namespace {
// Build a tiny hnswlib index for SnapshotManager save tests.
std::unique_ptr<hnswlib::HierarchicalNSW<float>> MakeIndex(hnswlib::L2Space* space,
                                                           int dim, int n) {
    auto idx = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        space, 1000, 16, 200, 100, true);
    for (int i = 0; i < n; ++i) {
        auto v = MakeVec(dim, i);
        idx->addPoint(v.data(), static_cast<hnswlib::labeltype>(i + 1));
    }
    return idx;
}
}  // namespace

TEST_F(PHnswRecoveryTest, SnapshotManager_SaveNullIndexRejected) {
    fs::create_directories(dir_);
    SnapshotManager mgr(dir_, 2);
    Status s = mgr.Save(nullptr, /*lsn=*/1, /*dim=*/kDim, /*M=*/16);
    EXPECT_FALSE(s.ok());  // null index → kInvalidArgument branch
    EXPECT_NE(s.message().find("null index"), std::string::npos);
}

TEST_F(PHnswRecoveryTest, SnapshotManager_KeepCountClampedToOne) {
    // keep_count < 1 is clamped to 1 (ctor branch). Two saves keep exactly one.
    fs::create_directories(dir_);
    hnswlib::L2Space space(kDim);
    auto index = MakeIndex(&space, kDim, 3);
    SnapshotManager mgr(dir_, /*keep_count=*/0);  // clamped to 1
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(mgr.Save(index.get(), static_cast<uint64_t>(i + 1), kDim, 16).ok());
    }
    int index_files = 0;
    for (const auto& e : fs::directory_iterator(dir_)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("hnsw_", 0) == 0 && n.size() >= 6 &&
            n.compare(n.size() - 6, 6, ".index") == 0) {
            ++index_files;
        }
    }
    EXPECT_EQ(index_files, 1);  // clamped keep_count=1
}

TEST_F(PHnswRecoveryTest, SnapshotManager_PeekLatestMeta) {
    fs::create_directories(dir_);
    hnswlib::L2Space space(kDim);
    auto index = MakeIndex(&space, kDim, 7);
    SnapshotManager mgr(dir_, 2);

    // No snapshot yet → PeekLatestMeta returns false (the empty-list branch).
    SnapshotManager::SnapshotMeta meta;
    EXPECT_FALSE(mgr.PeekLatestMeta(&meta));

    ASSERT_TRUE(mgr.Save(index.get(), /*lsn=*/42, /*dim=*/kDim, /*M=*/16).ok());
    ASSERT_TRUE(mgr.PeekLatestMeta(&meta));
    EXPECT_EQ(meta.lsn, 42u);
    EXPECT_EQ(meta.entry_count, 7u);
    EXPECT_EQ(meta.dim, kDim);
    EXPECT_EQ(meta.M, 16);
}

TEST_F(PHnswRecoveryTest, SnapshotManager_LatestSnapshotLsn_NoneIsZero) {
    fs::create_directories(dir_);
    SnapshotManager mgr(dir_, 2);
    EXPECT_EQ(mgr.LatestSnapshotLsn(), 0u);  // no snapshot → 0 branch
}

TEST_F(PHnswRecoveryTest, SnapshotManager_ListIgnoresUnrelatedAndTmpFiles) {
    // ListSnapshotsNewestFirst must skip: non-matching names, .tmp files,
    // hnsw_<non-numeric>.index, and the .index.meta sibling. We drop a mix of
    // such files, save one real snapshot, and confirm Load picks only the real one.
    fs::create_directories(dir_);
    auto touch = [&](const std::string& name) {
        std::ofstream(fs::path(dir_) / name) << "x";
    };
    touch("README.txt");                  // wrong prefix/suffix
    touch("hnsw_.index");                 // empty timestamp
    touch("hnsw_abc.index");              // non-numeric timestamp
    touch("hnsw_123.index.tmp");          // .tmp (interrupted save)
    touch("hnsw_999.index.meta");         // orphan meta, no matching .index entry name

    hnswlib::L2Space space(kDim);
    auto index = MakeIndex(&space, kDim, 4);
    SnapshotManager mgr(dir_, 2);
    ASSERT_TRUE(mgr.Save(index.get(), /*lsn=*/5, /*dim=*/kDim, /*M=*/16).ok());

    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 0;
    ASSERT_TRUE(mgr.Load(&space, 1000, &loaded, &lsn).ok());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(lsn, 5u);  // only the real snapshot loaded; junk ignored
}

TEST_F(PHnswRecoveryTest, SnapshotManager_OrphanIndexNoMeta_TreatedAsEmpty) {
    // An .index with NO valid .meta is not a committed snapshot: Load skips it and,
    // since nothing else exists, returns Ok with a null index (the any_seen==false
    // "treat as no snapshot" branch).
    fs::create_directories(dir_);
    std::ofstream(fs::path(dir_) / "hnsw_100.index") << "orphan-index-bytes";
    // No hnsw_100.index.meta written.

    hnswlib::L2Space space(kDim);
    SnapshotManager mgr(dir_, 2);
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 0;
    Status s = mgr.Load(&space, 1000, &loaded, &lsn);
    EXPECT_TRUE(s.ok());        // orphan → no snapshot, not an error
    EXPECT_EQ(loaded, nullptr);
    EXPECT_EQ(lsn, 0u);
}

TEST_F(PHnswRecoveryTest, SnapshotManager_ShortMeta_Skipped) {
    // A .meta shorter than the 24B footer is rejected (meta.size() != kFooterSize),
    // so the snapshot is not committed → Load treats it as no snapshot.
    fs::create_directories(dir_);
    std::ofstream(fs::path(dir_) / "hnsw_200.index") << "index-bytes";
    std::ofstream(fs::path(dir_) / "hnsw_200.index.meta") << "short";  // < 24 bytes

    hnswlib::L2Space space(kDim);
    SnapshotManager mgr(dir_, 2);
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 0;
    Status s = mgr.Load(&space, 1000, &loaded, &lsn);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(loaded, nullptr);
    // PeekLatestMeta / LatestSnapshotLsn likewise skip the short meta.
    SnapshotManager::SnapshotMeta meta;
    EXPECT_FALSE(mgr.PeekLatestMeta(&meta));
    EXPECT_EQ(mgr.LatestSnapshotLsn(), 0u);
}

TEST_F(PHnswRecoveryTest, SnapshotManager_CleanRemovesStaleTmp) {
    // CleanOldSnapshots removes hnsw_*.index.tmp leftovers from interrupted saves.
    fs::create_directories(dir_);
    const fs::path tmp = fs::path(dir_) / "hnsw_777.index.tmp";
    std::ofstream(tmp) << "interrupted";
    ASSERT_TRUE(fs::exists(tmp));

    SnapshotManager mgr(dir_, 2);
    mgr.CleanOldSnapshots();
    EXPECT_FALSE(fs::exists(tmp));  // stale .tmp reaped
}

TEST_F(PHnswRecoveryTest, SnapshotManager_LoadNewestThenFallbackToOlder) {
    // Two valid snapshots; corrupt the NEWER one's .index body. Load tries newest
    // first (CRC mismatch → "trying older" branch), then succeeds on the older.
    fs::create_directories(dir_);
    hnswlib::L2Space space(kDim);
    auto index = MakeIndex(&space, kDim, 3);
    SnapshotManager mgr(dir_, /*keep_count=*/5);  // keep both
    ASSERT_TRUE(mgr.Save(index.get(), /*lsn=*/1, kDim, 16).ok());
    // Ensure a later timestamp for the second save (Save stamps by ms).
    for (int i = 0; i < 50; ++i) {
        if (mgr.Save(index.get(), /*lsn=*/2, kDim, 16).ok()) {}
    }

    // Collect .index files by descending timestamp; corrupt the newest one.
    std::vector<fs::path> indexes;
    for (const auto& e : fs::directory_iterator(dir_)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("hnsw_", 0) == 0 && n.size() >= 6 &&
            n.compare(n.size() - 6, 6, ".index") == 0) {
            indexes.push_back(e.path());
        }
    }
    ASSERT_GE(indexes.size(), 2u);
    std::sort(indexes.begin(), indexes.end());  // lexicographic ~ ascending ts
    const fs::path newest = indexes.back();
    {
        int fd = ::open(newest.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        uint8_t b = 0;
        ASSERT_EQ(::pread(fd, &b, 1, 0), 1);
        b ^= 0xFF;
        ASSERT_EQ(::pwrite(fd, &b, 1, 0), 1);
        ::close(fd);
    }
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 0;
    Status s = mgr.Load(&space, 1000, &loaded, &lsn);
    ASSERT_TRUE(s.ok()) << s.message();  // fell back to the older valid snapshot
    EXPECT_NE(loaded, nullptr);
}

// --- round 2: deeper fallback / all-corrupt combinations -------------------

namespace {
// Collect hnsw_*.index paths sorted ascending by timestamp (lexicographic works
// for fixed-width ms stamps).
std::vector<fs::path> IndexFilesAscending(const std::string& dir) {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("hnsw_", 0) == 0 && n.size() >= 6 &&
            n.compare(n.size() - 6, 6, ".index") == 0) {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
void FlipFirstByte(const fs::path& p) {
    int fd = ::open(p.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    uint8_t b = 0;
    ASSERT_EQ(::pread(fd, &b, 1, 0), 1);
    b ^= 0xFF;
    ASSERT_EQ(::pwrite(fd, &b, 1, 0), 1);
    ::close(fd);
}
}  // namespace

TEST_F(PHnswRecoveryTest, SnapshotManager_NewestIndexMissing_FallsBackToOlder) {
    // Two committed snapshots; delete the NEWER .index but keep its (valid) .meta.
    // Load reads the newest .meta, fails ReadWholeFile on the missing .index
    // (the "snapshot unreadable; trying older" branch), then loads the older.
    fs::create_directories(dir_);
    hnswlib::L2Space space(kDim);
    auto index = MakeIndex(&space, kDim, 3);
    SnapshotManager mgr(dir_, /*keep_count=*/5);
    ASSERT_TRUE(mgr.Save(index.get(), /*lsn=*/1, kDim, 16).ok());
    for (int i = 0; i < 50; ++i) {
        if (mgr.Save(index.get(), /*lsn=*/2, kDim, 16).ok()) {}
    }
    auto indexes = IndexFilesAscending(dir_);
    ASSERT_GE(indexes.size(), 2u);
    std::error_code ec;
    fs::remove(indexes.back(), ec);  // newest .index gone, .meta remains
    ASSERT_FALSE(ec);

    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 0;
    Status s = mgr.Load(&space, 1000, &loaded, &lsn);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(loaded, nullptr);  // older snapshot served
}

TEST_F(PHnswRecoveryTest, SnapshotManager_AllSnapshotsCorrupt_MultiSet) {
    // Two committed snapshots, BOTH .index bodies corrupted. Load tries newest →
    // CRC mismatch → older → CRC mismatch → any_seen==true with nothing loadable
    // → CX_ERR_PHNSW_SNAPSHOT_CORRUPTED (the loop-exhausted error branch).
    fs::create_directories(dir_);
    hnswlib::L2Space space(kDim);
    auto index = MakeIndex(&space, kDim, 4);
    SnapshotManager mgr(dir_, /*keep_count=*/5);
    ASSERT_TRUE(mgr.Save(index.get(), /*lsn=*/1, kDim, 16).ok());
    for (int i = 0; i < 50; ++i) {
        if (mgr.Save(index.get(), /*lsn=*/2, kDim, 16).ok()) {}
    }
    auto indexes = IndexFilesAscending(dir_);
    ASSERT_GE(indexes.size(), 2u);
    for (const auto& p : indexes) FlipFirstByte(p);  // corrupt every .index

    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 0;
    Status s = mgr.Load(&space, 1000, &loaded, &lsn);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(loaded, nullptr);
    EXPECT_NE(s.message().find("CX_ERR_PHNSW_SNAPSHOT_CORRUPTED"), std::string::npos);
}

TEST_F(PHnswRecoveryTest, Recover_PropagatesCorruptSnapshotError) {
    // PHnsw::Recover() → RecoverInternal → SnapshotManager::Load. With the only
    // snapshot's .index corrupted (and the WAL already truncated by Snapshot),
    // Load returns CX_ERR_PHNSW_SNAPSHOT_CORRUPTED and Recover surfaces it (the
    // RecoverInternal "snapshot load failed" propagation branch).
    PHnsw idx(dir_, config_);
    for (int i = 0; i < 6; ++i) {
        auto v = MakeVec(kDim, i);
        ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
    }
    ASSERT_TRUE(idx.Snapshot().ok());  // truncates the WAL; data now only in snapshot

    // Corrupt the snapshot .index body so its CRC no longer matches the footer.
    for (const auto& e : fs::directory_iterator(dir_)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("hnsw_", 0) == 0 && n.size() >= 6 &&
            n.compare(n.size() - 6, 6, ".index") == 0) {
            int fd = ::open(e.path().c_str(), O_RDWR);
            ASSERT_GE(fd, 0);
            uint8_t b = 0;
            ASSERT_EQ(::pread(fd, &b, 1, 0), 1);
            b ^= 0xFF;
            ASSERT_EQ(::pwrite(fd, &b, 1, 0), 1);
            ::close(fd);
        }
    }
    Status st = idx.Recover();
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_PHNSW_SNAPSHOT_CORRUPTED"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::store
