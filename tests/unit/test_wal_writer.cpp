// Index S2 — WalWriter file-layer tests (design § 6 S2 test matrix).
//
// Append single/batch, mixed INSERT/DELETE, ReadAll round-trip, Truncate, header
// entry_count/committed_lsn updates, empty WAL, large file, corrupted trailing
// entry (tail truncation), corrupted header (degraded recovery), reopen
// persistence, and auto-create on a missing path.

#include "cortrix/store/phnsw/wal_writer.h"

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "cortrix/store/phnsw/wal_entry.h"

namespace cortrix::store {
namespace {

namespace fs = std::filesystem;

std::vector<float> MakeVec(int dim, int seed) {
    std::vector<float> v(static_cast<size_t>(dim));
    for (int i = 0; i < dim; ++i) {
        v[static_cast<size_t>(i)] = static_cast<float>((seed * 11 + i) % 233) * 0.017f;
    }
    return v;
}

class WalWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("cortrix_wal_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        path_ = (dir_ / "hnsw.wal").string();
    }
    void TearDown() override { fs::remove_all(dir_); }

    static constexpr int kDim = 8;
    fs::path dir_;
    std::string path_;
};

TEST_F(WalWriterTest, FileNotExist_CreatesNewWal) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok()) << w.status().message();
    EXPECT_TRUE(fs::exists(path_));
    // Header-only file == kHeaderSize bytes.
    EXPECT_EQ(fs::file_size(path_), WalWriter::kHeaderSize);
    auto stats = w.value()->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats.value().entry_count, 0u);
}

TEST_F(WalWriterTest, EmptyWal_ReadAllReturnsNone) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto recs = w.value()->ReadAll(kDim);
    ASSERT_TRUE(recs.ok());
    EXPECT_TRUE(recs.value().empty());
}

TEST_F(WalWriterTest, AppendSingle) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto v = MakeVec(kDim, 1);
    ASSERT_TRUE(w.value()->Append(WalEntry::SerializeInsert(100, v.data(), kDim)).ok());

    auto recs = w.value()->ReadAll(kDim);
    ASSERT_TRUE(recs.ok());
    ASSERT_EQ(recs.value().size(), 1u);
    EXPECT_EQ(recs.value()[0].block_id, 100u);
    EXPECT_EQ(recs.value()[0].type, WalEntry::Type::kInsert);
}

TEST_F(WalWriterTest, AppendBatch_TenInserts) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    std::vector<std::vector<uint8_t>> recs;
    std::vector<std::vector<float>> vecs;
    for (int i = 0; i < 10; ++i) vecs.push_back(MakeVec(kDim, i));
    for (int i = 0; i < 10; ++i) {
        recs.push_back(WalEntry::SerializeInsert(static_cast<uint64_t>(i + 1),
                                                 vecs[static_cast<size_t>(i)].data(), kDim));
    }
    ASSERT_TRUE(w.value()->AppendBatch(recs).ok());

    auto read = w.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), 10u);
}

TEST_F(WalWriterTest, MixedTypes) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto v = MakeVec(kDim, 5);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(w.value()->Append(
            WalEntry::SerializeInsert(static_cast<uint64_t>(i), v.data(), kDim)).ok());
    }
    ASSERT_TRUE(w.value()->Append(WalEntry::SerializeDelete(1)).ok());
    ASSERT_TRUE(w.value()->Append(WalEntry::SerializeDelete(3)).ok());

    auto read = w.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    ASSERT_EQ(read.value().size(), 7u);
    int inserts = 0, deletes = 0;
    for (const auto& e : read.value()) {
        if (e.type == WalEntry::Type::kInsert) ++inserts;
        else ++deletes;
    }
    EXPECT_EQ(inserts, 5);
    EXPECT_EQ(deletes, 2);
}

TEST_F(WalWriterTest, HeaderUpdate_EntryCount) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto v = MakeVec(kDim, 1);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(w.value()->Append(
            WalEntry::SerializeInsert(static_cast<uint64_t>(i), v.data(), kDim)).ok());
    }
    auto stats = w.value()->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats.value().entry_count, 5u);
    EXPECT_EQ(stats.value().committed_lsn, 5u);  // committed_lsn == entry_count after clean flush
}

TEST_F(WalWriterTest, Truncate_ClearsRecordsKeepsHeader) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto v = MakeVec(kDim, 1);
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(w.value()->Append(
            WalEntry::SerializeInsert(static_cast<uint64_t>(i), v.data(), kDim)).ok());
    }
    ASSERT_TRUE(w.value()->Truncate().ok());

    auto read = w.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    EXPECT_TRUE(read.value().empty());
    EXPECT_EQ(fs::file_size(path_), WalWriter::kHeaderSize);
    auto stats = w.value()->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats.value().entry_count, 0u);
}

TEST_F(WalWriterTest, LargeFile_ThousandRecords) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto v = MakeVec(kDim, 2);
    std::vector<std::vector<uint8_t>> recs;
    for (int i = 0; i < 1000; ++i) {
        recs.push_back(WalEntry::SerializeInsert(static_cast<uint64_t>(i), v.data(), kDim));
    }
    ASSERT_TRUE(w.value()->AppendBatch(recs).ok());
    auto read = w.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    ASSERT_EQ(read.value().size(), 1000u);
    EXPECT_EQ(read.value()[999].block_id, 999u);
}

TEST_F(WalWriterTest, Persistence_AcrossReopen) {
    {
        auto w = WalWriter::Open(path_, kDim);
        ASSERT_TRUE(w.ok());
        auto v = MakeVec(kDim, 1);
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(w.value()->Append(
                WalEntry::SerializeInsert(static_cast<uint64_t>(i + 1), v.data(), kDim)).ok());
        }
    }  // writer destroyed → fd closed
    // Reopen and confirm records + header survive.
    auto w2 = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w2.ok());
    auto stats = w2.value()->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats.value().entry_count, 3u);
    auto read = w2.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), 3u);
}

TEST_F(WalWriterTest, CorruptedTrailingEntry_TruncatedOnRead) {
    {
        auto w = WalWriter::Open(path_, kDim);
        ASSERT_TRUE(w.ok());
        auto v = MakeVec(kDim, 1);
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(w.value()->Append(
                WalEntry::SerializeInsert(static_cast<uint64_t>(i + 1), v.data(), kDim)).ok());
        }
    }
    // Corrupt the last byte of the file (inside record #5's CRC/payload).
    {
        int fd = ::open(path_.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        off_t end = ::lseek(fd, 0, SEEK_END);
        uint8_t b = 0;
        ASSERT_EQ(::pread(fd, &b, 1, end - 1), 1);
        b ^= 0xFF;
        ASSERT_EQ(::pwrite(fd, &b, 1, end - 1), 1);
        ::close(fd);
    }
    // ReadAll should return the first 4 valid records and truncate the bad tail.
    auto w2 = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w2.ok());
    auto read = w2.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), 4u);
    // After repair, the header count matches and reopening is consistent.
    auto stats = w2.value()->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats.value().entry_count, 4u);
}

TEST_F(WalWriterTest, CorruptedHeader_RejectedOnOpen) {
    {
        auto w = WalWriter::Open(path_, kDim);
        ASSERT_TRUE(w.ok());
    }
    // Corrupt the magic.
    {
        int fd = ::open(path_.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        uint8_t bad = 'X';
        ASSERT_EQ(::pwrite(fd, &bad, 1, 0), 1);
        ::close(fd);
    }
    auto w2 = WalWriter::Open(path_, kDim);
    EXPECT_FALSE(w2.ok());
    EXPECT_NE(w2.status().message().find("CX_ERR_PHNSW_WAL_CORRUPTED"), std::string::npos);
}

TEST_F(WalWriterTest, DimMismatchOnReopen_Rejected) {
    {
        auto w = WalWriter::Open(path_, kDim);
        ASSERT_TRUE(w.ok());
    }
    // Reopen declaring a different dim → header dim check fails.
    auto w2 = WalWriter::Open(path_, kDim * 2);
    EXPECT_FALSE(w2.ok());
}

// --- branch-coverage supplements (S2 error/edge paths) --------------------

TEST_F(WalWriterTest, Open_RejectsNonPositiveDim) {
    // dim <= 0 short-circuits before any open() (kVectorDimMismatch).
    auto w0 = WalWriter::Open(path_, 0);
    EXPECT_FALSE(w0.ok());
    EXPECT_NE(w0.status().message().find("dim must be > 0"), std::string::npos);
    auto wn = WalWriter::Open(path_, -4);
    EXPECT_FALSE(wn.ok());
    // Neither attempt should have created the file.
    EXPECT_FALSE(fs::exists(path_));
}

TEST_F(WalWriterTest, Open_RejectsUnopenablePath) {
    // A path whose parent does not exist makes open(O_CREAT) fail (kWalWriteFailed).
    const std::string bad = (dir_ / "no_such_subdir" / "hnsw.wal").string();
    auto w = WalWriter::Open(bad, kDim);
    EXPECT_FALSE(w.ok());
}

TEST_F(WalWriterTest, Open_FileSmallerThanHeader_Corrupted) {
    // A non-empty file shorter than the 32B header → CX_ERR_PHNSW_WAL_CORRUPTED
    // (the "file smaller than header" branch, distinct from the empty-file path).
    {
        int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
        ASSERT_GE(fd, 0);
        const char junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        ASSERT_EQ(::pwrite(fd, junk, sizeof(junk), 0), static_cast<ssize_t>(sizeof(junk)));
        ::close(fd);
    }
    auto w = WalWriter::Open(path_, kDim);
    EXPECT_FALSE(w.ok());
    EXPECT_NE(w.status().message().find("file smaller than header"), std::string::npos);
}

TEST_F(WalWriterTest, Open_BadVersion_Corrupted) {
    // Keep the magic valid but corrupt the version field (offset 4) so the
    // version-mismatch branch fires (not the magic branch).
    {
        auto w = WalWriter::Open(path_, kDim);
        ASSERT_TRUE(w.ok());
    }
    {
        int fd = ::open(path_.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        uint8_t v = 0xEE;  // version != kVersion(1)
        ASSERT_EQ(::pwrite(fd, &v, 1, 4), 1);
        ::close(fd);
    }
    auto w2 = WalWriter::Open(path_, kDim);
    EXPECT_FALSE(w2.ok());
    EXPECT_NE(w2.status().message().find("unsupported version"), std::string::npos);
}

TEST_F(WalWriterTest, Open_HeaderCrcOnlyCorrupt_DegradedRecovery) {
    // Corrupt a CRC-covered byte that is NOT magic/version/dim (the committed_lsn
    // field, offset 18) so magic+version+dim all pass but the header CRC fails.
    // Open then succeeds with header_trusted_=false; ReadAll re-derives counts and
    // rewrites a clean header (the degraded-recovery branch in Open + ReadAll).
    {
        auto w = WalWriter::Open(path_, kDim);
        ASSERT_TRUE(w.ok());
        auto v = MakeVec(kDim, 7);
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(w.value()->Append(
                WalEntry::SerializeInsert(static_cast<uint64_t>(i + 1), v.data(), kDim)).ok());
        }
    }
    {
        int fd = ::open(path_.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        uint8_t b = 0;
        ASSERT_EQ(::pread(fd, &b, 1, 18), 1);  // committed_lsn byte (CRC-covered)
        b ^= 0xFF;
        ASSERT_EQ(::pwrite(fd, &b, 1, 18), 1);
        ::close(fd);
    }
    auto w2 = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w2.ok());  // header CRC fail is non-fatal — counts re-derived on read
    auto read = w2.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), 3u);  // records intact, recovered by scan
    // Reopening now sees a trusted, clean header with the re-derived count.
    auto w3 = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w3.ok());
    auto stats = w3.value()->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats.value().entry_count, 3u);
}

TEST_F(WalWriterTest, AppendBatch_EmptyIsNoOp) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    // Empty batch hits the early-return branch (no write, no header rewrite).
    EXPECT_TRUE(w.value()->AppendBatch({}).ok());
    EXPECT_EQ(fs::file_size(path_), WalWriter::kHeaderSize);
    auto stats = w.value()->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats.value().entry_count, 0u);
}

TEST_F(WalWriterTest, WriteBatchThenSync_IWalSinkPath) {
    // The IWalSink path used by GroupCommitWriter: WriteBatch writes records +
    // header WITHOUT syncing, then a single Sync() makes them durable. Empty
    // WriteBatch is a no-op (WriteRecordsLocked early return).
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    EXPECT_TRUE(w.value()->WriteBatch({}).ok());  // empty → no-op branch

    auto v = MakeVec(kDim, 3);
    std::vector<std::vector<uint8_t>> recs;
    for (int i = 0; i < 4; ++i) {
        recs.push_back(WalEntry::SerializeInsert(static_cast<uint64_t>(i + 1), v.data(), kDim));
    }
    ASSERT_TRUE(w.value()->WriteBatch(recs).ok());
    ASSERT_TRUE(w.value()->Sync().ok());

    auto read = w.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), 4u);
    EXPECT_EQ(w.value()->entry_count(), 4u);
    EXPECT_EQ(w.value()->committed_lsn(), 4u);
}

TEST_F(WalWriterTest, CommittedLsnMonotonicAcrossTruncate) {
    // Truncate clears entry_count but PRESERVES committed_lsn (monotonic), so
    // post-truncate appends continue the ascending LSN sequence.
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto v = MakeVec(kDim, 1);
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(w.value()->Append(
            WalEntry::SerializeInsert(static_cast<uint64_t>(i + 1), v.data(), kDim)).ok());
    }
    EXPECT_EQ(w.value()->committed_lsn(), 6u);
    ASSERT_TRUE(w.value()->Truncate().ok());
    EXPECT_EQ(w.value()->entry_count(), 0u);
    EXPECT_EQ(w.value()->committed_lsn(), 6u);  // preserved
    // Append two more: LSN keeps ascending from the preserved base.
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(w.value()->Append(
            WalEntry::SerializeInsert(static_cast<uint64_t>(100 + i), v.data(), kDim)).ok());
    }
    EXPECT_EQ(w.value()->entry_count(), 2u);
    EXPECT_EQ(w.value()->committed_lsn(), 8u);
}

TEST_F(WalWriterTest, ReadAll_ExpectedDimZeroUsesDeclaredDim) {
    // expected_dim <= 0 selects the declared (file) dim_ for the per-record dim
    // cross-check (the dim_check ternary's else branch).
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto v = MakeVec(kDim, 2);
    ASSERT_TRUE(w.value()->Append(WalEntry::SerializeInsert(1, v.data(), kDim)).ok());
    auto read = w.value()->ReadAll(/*expected_dim=*/0);  // 0 → use dim_
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), 1u);
}

TEST_F(WalWriterTest, GetStats_FileSizeReflectsAppends) {
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto before = w.value()->GetStats();
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(before.value().file_size_bytes, WalWriter::kHeaderSize);
    auto v = MakeVec(kDim, 1);
    ASSERT_TRUE(w.value()->Append(WalEntry::SerializeInsert(1, v.data(), kDim)).ok());
    auto after = w.value()->GetStats();
    ASSERT_TRUE(after.ok());
    EXPECT_GT(after.value().file_size_bytes, WalWriter::kHeaderSize);
}

// --- ReadAll tail-corruption sub-branch matrix (round 2) -------------------
//
// All three garbage modes make DeserializeFrom fail on the trailing bytes → the
// scan breaks, the file is ftruncate'd to the last valid record, the header is
// rewritten with the re-derived count, and a durable sync follows. They differ
// in *which* DeserializeFrom arm rejects the tail, exercising distinct paths
// into the same repair block.

namespace {
// Append raw bytes to the end of the WAL file (after the writer is closed).
void AppendRawBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    off_t end = ::lseek(fd, 0, SEEK_END);
    ASSERT_GE(end, 0);
    ASSERT_EQ(::pwrite(fd, bytes.data(), bytes.size(), end),
              static_cast<ssize_t>(bytes.size()));
    ::close(fd);
}
}  // namespace

TEST_F(WalWriterTest, ReadAll_TailGarbageBelowMinimum_Truncated) {
    // 3 valid records, then a 9-byte tail: a valid 4B entry_size prefix declaring
    // entry_size=5 (below the fixed body+CRC minimum). DeserializeFrom rejects it
    // ("entry_size below minimum") → tail dropped, 3 recovered.
    {
        auto w = WalWriter::Open(path_, kDim);
        ASSERT_TRUE(w.ok());
        auto v = MakeVec(kDim, 1);
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(w.value()->Append(
                WalEntry::SerializeInsert(static_cast<uint64_t>(i + 1), v.data(), kDim)).ok());
        }
    }
    AppendRawBytes(path_, {0x05, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE});

    auto w2 = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w2.ok());
    auto read = w2.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), 3u);  // garbage tail truncated
    auto stats = w2.value()->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats.value().entry_count, 3u);  // header re-derived after repair
}

TEST_F(WalWriterTest, ReadAll_TailShortFrame_Truncated) {
    // 2 valid records, then a partial frame: a 4B prefix declaring a large
    // entry_size but with too few following bytes → "frame extends past buffer".
    {
        auto w = WalWriter::Open(path_, kDim);
        ASSERT_TRUE(w.ok());
        auto v = MakeVec(kDim, 2);
        for (int i = 0; i < 2; ++i) {
            ASSERT_TRUE(w.value()->Append(
                WalEntry::SerializeInsert(static_cast<uint64_t>(i + 1), v.data(), kDim)).ok());
        }
    }
    // entry_size = 100 but only 3 payload bytes follow → frame past buffer.
    AppendRawBytes(path_, {0x64, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33});

    auto w2 = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w2.ok());
    auto read = w2.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), 2u);
    // Repair truncates the file to the last valid record; reopening is consistent.
    auto w3 = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w3.ok());
    auto read3 = w3.value()->ReadAll(kDim);
    ASSERT_TRUE(read3.ok());
    EXPECT_EQ(read3.value().size(), 2u);
}

TEST_F(WalWriterTest, ReadAll_TailShorterThanSizePrefix_Truncated) {
    // 1 valid record, then only 2 trailing bytes (< 4B entry_size prefix) →
    // "truncated entry_size" arm.
    {
        auto w = WalWriter::Open(path_, kDim);
        ASSERT_TRUE(w.ok());
        auto v = MakeVec(kDim, 3);
        ASSERT_TRUE(w.value()->Append(WalEntry::SerializeInsert(1, v.data(), kDim)).ok());
    }
    AppendRawBytes(path_, {0xDE, 0xAD});

    auto w2 = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w2.ok());
    auto read = w2.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), 1u);
    auto stats = w2.value()->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats.value().entry_count, 1u);
}

TEST_F(WalWriterTest, ReadAll_DeleteTailRecordRoundTrips) {
    // A DELETE as the trailing record exercises the DELETE parse arm inside the
    // scan loop (the prior tests all end on INSERT/garbage).
    auto w = WalWriter::Open(path_, kDim);
    ASSERT_TRUE(w.ok());
    auto v = MakeVec(kDim, 5);
    ASSERT_TRUE(w.value()->Append(WalEntry::SerializeInsert(1, v.data(), kDim)).ok());
    ASSERT_TRUE(w.value()->Append(WalEntry::SerializeDelete(1)).ok());
    auto read = w.value()->ReadAll(kDim);
    ASSERT_TRUE(read.ok());
    ASSERT_EQ(read.value().size(), 2u);
    EXPECT_EQ(read.value()[1].type, WalEntry::Type::kDelete);
}

}  // namespace
}  // namespace cortrix::store
