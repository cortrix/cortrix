#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "cortrix/store/pending_entry.h"
#include "cortrix/store/pending_log_writer.h"

namespace cortrix::store {
namespace {

using State = PendingEntry::State;

// Per-test scratch directory under the system temp dir; removed on teardown.
class PendingLogWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/cortrix_pwl_XXXXXX";
        char* d = ::mkdtemp(tmpl);
        ASSERT_NE(d, nullptr) << "mkdtemp failed";
        dir_ = d;
        wal_path_ = dir_ + "/pending.wal";
    }
    void TearDown() override {
        // Best-effort cleanup; ignore if a test already removed the file.
        ::unlink(wal_path_.c_str());
        ::unlink((wal_path_ + ".compact.tmp").c_str());
        ::rmdir(dir_.c_str());
    }

    static PendingEntry Pending(uint64_t txn, const std::string& doc,
                                std::vector<BlockId> blocks) {
        PendingEntry e;
        e.state = State::kPending;
        e.txn_id = txn;
        e.timestamp_ms = static_cast<int64_t>(txn) * 1000;
        e.doc_id = doc;
        e.block_ids = std::move(blocks);
        return e;
    }
    static PendingEntry Terminal(State s, uint64_t txn) {
        PendingEntry e;
        e.state = s;
        e.txn_id = txn;
        e.timestamp_ms = static_cast<int64_t>(txn) * 1000 + 1;
        return e;
    }

    std::string dir_;
    std::string wal_path_;
};

TEST_F(PendingLogWriterTest, OpenCreatesHeaderOnlyFile) {
    auto w = PendingLogWriter::Open(wal_path_, /*max_batch=*/16, /*timeout_ms=*/5);
    ASSERT_TRUE(w.ok()) << w.status().message();

    // File should be exactly the 32B header, no records yet.
    off_t sz = ::lseek(::open(wal_path_.c_str(), O_RDONLY), 0, SEEK_END);
    EXPECT_EQ(sz, static_cast<off_t>(PendingLogWriter::kHeaderSize));

    auto all = w.value()->ReadAll();
    ASSERT_TRUE(all.ok());
    EXPECT_TRUE(all->empty());
}

TEST_F(PendingLogWriterTest, EmptyWalReadAllReturnsNothing) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto all = w.value()->ReadAll();
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all->size(), 0u);
}

TEST_F(PendingLogWriterTest, AppendPendingThenReadAll) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());

    ASSERT_TRUE(w.value()->Append(Pending(1, "doc-1", {10, 11, 12})).ok());

    auto all = w.value()->ReadAll();
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all->size(), 1u);
    EXPECT_EQ((*all)[0].state, State::kPending);
    EXPECT_EQ((*all)[0].txn_id, 1u);
    EXPECT_EQ((*all)[0].doc_id, "doc-1");
    EXPECT_EQ((*all)[0].block_ids, (std::vector<BlockId>{10, 11, 12}));
}

TEST_F(PendingLogWriterTest, AppendMixedPreservesOrderAndTypes) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto* pwl = w.value().get();

    std::vector<PendingEntry> batch;
    for (BlockId i = 1; i <= 5; ++i) batch.push_back(Pending(i, "doc", {i}));
    for (BlockId i = 1; i <= 3; ++i) batch.push_back(Terminal(State::kCommitted, i));
    for (BlockId i = 4; i <= 5; ++i) batch.push_back(Terminal(State::kRollback, i));
    ASSERT_TRUE(pwl->AppendBatch(batch).ok());

    auto all = pwl->ReadAll();
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all->size(), 10u);
    int pending = 0, committed = 0, rollback = 0;
    for (const auto& e : *all) {
        if (e.state == State::kPending) ++pending;
        else if (e.state == State::kCommitted) ++committed;
        else ++rollback;
    }
    EXPECT_EQ(pending, 5);
    EXPECT_EQ(committed, 3);
    EXPECT_EQ(rollback, 2);
}

TEST_F(PendingLogWriterTest, StatsCountsLiveBacklog) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto* pwl = w.value().get();

    // 5 PENDING, then finalize txns 1..3 → 2 still live (txn 4,5).
    for (BlockId i = 1; i <= 5; ++i) ASSERT_TRUE(pwl->Append(Pending(i, "doc", {i})).ok());
    ASSERT_TRUE(pwl->Append(Terminal(State::kCommitted, 1)).ok());
    ASSERT_TRUE(pwl->Append(Terminal(State::kCommitted, 2)).ok());
    ASSERT_TRUE(pwl->Append(Terminal(State::kCommitted, 3)).ok());

    auto s = pwl->GetStats();
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s->pending, 2u);      // txn 4, 5 not finalized
    EXPECT_EQ(s->committed, 3u);
    EXPECT_EQ(s->rollback, 0u);
}

TEST_F(PendingLogWriterTest, CompactRemovesAllFinalizedTxns) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto* pwl = w.value().get();

    // 5 PENDING (txn 1-5) + COMMITTED 1-3 + ROLLBACK 4-5 → all terminal.
    for (BlockId i = 1; i <= 5; ++i) ASSERT_TRUE(pwl->Append(Pending(i, "d", {i})).ok());
    for (BlockId i = 1; i <= 3; ++i) ASSERT_TRUE(pwl->Append(Terminal(State::kCommitted, i)).ok());
    for (BlockId i = 4; i <= 5; ++i) ASSERT_TRUE(pwl->Append(Terminal(State::kRollback, i)).ok());

    ASSERT_TRUE(pwl->Compact().ok());

    auto all = pwl->ReadAll();
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all->size(), 0u) << "every txn finalized → nothing survives compaction";

    // Appends must still work against the reopened, compacted file.
    ASSERT_TRUE(pwl->Append(Pending(6, "d6", {6})).ok());
    auto all2 = pwl->ReadAll();
    ASSERT_TRUE(all2.ok());
    ASSERT_EQ(all2->size(), 1u);
    EXPECT_EQ((*all2)[0].txn_id, 6u);
}

TEST_F(PendingLogWriterTest, CompactKeepsUnfinalizedPending) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto* pwl = w.value().get();

    // 5 PENDING (txn 1-5), finalize only 1 & 2 → txn 3,4,5 survive.
    for (BlockId i = 1; i <= 5; ++i) ASSERT_TRUE(pwl->Append(Pending(i, "d", {i, i + 100})).ok());
    ASSERT_TRUE(pwl->Append(Terminal(State::kCommitted, 1)).ok());
    ASSERT_TRUE(pwl->Append(Terminal(State::kRollback, 2)).ok());

    ASSERT_TRUE(pwl->Compact().ok());

    auto all = pwl->ReadAll();
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all->size(), 3u);
    for (const auto& e : *all) {
        EXPECT_EQ(e.state, State::kPending);
        EXPECT_GE(e.txn_id, 3u);
        EXPECT_LE(e.txn_id, 5u);
        EXPECT_EQ(e.block_ids.size(), 2u);  // PENDING payload survives intact
    }

    auto s = pwl->GetStats();
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s->pending, 3u);
    EXPECT_EQ(s->committed, 0u);  // terminal records dropped by compaction
    EXPECT_EQ(s->rollback, 0u);
}

TEST_F(PendingLogWriterTest, CorruptedTailTruncatedOnReadAll) {
    {
        auto w = PendingLogWriter::Open(wal_path_, 16, 5);
        ASSERT_TRUE(w.ok());
        auto* pwl = w.value().get();
        ASSERT_TRUE(pwl->Append(Pending(1, "d1", {1})).ok());
        ASSERT_TRUE(pwl->Append(Pending(2, "d2", {2})).ok());
        pwl->Shutdown();  // ensure everything is flushed before we poke the file
    }

    // Append 9 bytes of garbage to simulate a torn write at the tail.
    int fd = ::open(wal_path_.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    off_t end = ::lseek(fd, 0, SEEK_END);
    const uint8_t junk[9] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05};
    ASSERT_EQ(::pwrite(fd, junk, sizeof(junk), end), static_cast<ssize_t>(sizeof(junk)));
    ::close(fd);

    // Reopen: header still valid; ReadAll returns the 2 good records and
    // truncates the garbage tail.
    auto w2 = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w2.ok());
    auto all = w2.value()->ReadAll();
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all->size(), 2u);
    EXPECT_EQ((*all)[0].txn_id, 1u);
    EXPECT_EQ((*all)[1].txn_id, 2u);

    // The tail was physically truncated: file size == header + 2 records.
    off_t sz = ::lseek(::open(wal_path_.c_str(), O_RDONLY), 0, SEEK_END);
    PendingEntry probe = Pending(1, "d1", {1});
    const off_t rec_len = static_cast<off_t>(probe.Serialize().size());
    EXPECT_EQ(sz, static_cast<off_t>(PendingLogWriter::kHeaderSize) + 2 * rec_len);
}

TEST_F(PendingLogWriterTest, DurableAcrossReopen) {
    {
        auto w = PendingLogWriter::Open(wal_path_, 16, 5);
        ASSERT_TRUE(w.ok());
        ASSERT_TRUE(w.value()->Append(Pending(1, "persist", {7, 8})).ok());
        // destructor runs Shutdown() + close()
    }
    auto w2 = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w2.ok());
    auto all = w2.value()->ReadAll();
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all->size(), 1u);
    EXPECT_EQ((*all)[0].doc_id, "persist");
    EXPECT_EQ((*all)[0].block_ids, (std::vector<BlockId>{7, 8}));
}

TEST_F(PendingLogWriterTest, RejectsBadMagicHeader) {
    // Write a 32B file with wrong magic; Open must refuse it as corrupted.
    int fd = ::open(wal_path_.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(fd, 0);
    std::vector<uint8_t> bad(PendingLogWriter::kHeaderSize, 0);
    bad[0] = 'X'; bad[1] = 'X'; bad[2] = 'X'; bad[3] = 'X';
    ASSERT_EQ(::pwrite(fd, bad.data(), bad.size(), 0),
              static_cast<ssize_t>(bad.size()));
    ::close(fd);

    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    EXPECT_FALSE(w.ok());
    EXPECT_NE(w.status().message().find("CX_ERR_PWL_CORRUPTED"), std::string::npos);
}

// --- Branch coverage: header validation rejections on reopen. ----------------

// A file shorter than the 32B header is corrupt (Open size<kHeaderSize branch).
TEST_F(PendingLogWriterTest, RejectsFileSmallerThanHeader) {
    int fd = ::open(wal_path_.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(fd, 0);
    const uint8_t partial[8] = {'P', 'W', 'L', 'G', 0, 0, 0, 0};
    ASSERT_EQ(::pwrite(fd, partial, sizeof(partial), 0), static_cast<ssize_t>(sizeof(partial)));
    ::close(fd);

    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    EXPECT_FALSE(w.ok());
    EXPECT_NE(w.status().message().find("CX_ERR_PWL_CORRUPTED"), std::string::npos);
}

// Correct magic but a version the writer does not understand → corrupted.
TEST_F(PendingLogWriterTest, RejectsUnsupportedVersion) {
    // Create a valid header-only file first, then patch the version field (bytes
    // [4,6)) and recompute nothing — the CRC will also mismatch, but the version
    // check runs first (Open order: magic → version → crc).
    {
        auto w = PendingLogWriter::Open(wal_path_, 16, 5);
        ASSERT_TRUE(w.ok());
        w.value()->Shutdown();
    }
    int fd = ::open(wal_path_.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    const uint8_t bad_ver[2] = {0xFF, 0xFF};  // version 65535 != kVersion
    ASSERT_EQ(::pwrite(fd, bad_ver, sizeof(bad_ver), 4), 2);
    ::close(fd);

    auto w2 = PendingLogWriter::Open(wal_path_, 16, 5);
    EXPECT_FALSE(w2.ok());
    EXPECT_NE(w2.status().message().find("CX_ERR_PWL_CORRUPTED"), std::string::npos);
}

// Right magic + version but a corrupted reserved region → header CRC mismatch.
TEST_F(PendingLogWriterTest, RejectsHeaderCrcMismatch) {
    {
        auto w = PendingLogWriter::Open(wal_path_, 16, 5);
        ASSERT_TRUE(w.ok());
        w.value()->Shutdown();
    }
    int fd = ::open(wal_path_.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    // Flip a reserved byte (offset 10, inside [6,28)) so it no longer matches the
    // stored CRC at [28,32). Magic + version stay valid → only the CRC check trips.
    const uint8_t flip = 0x5A;
    ASSERT_EQ(::pwrite(fd, &flip, 1, 10), 1);
    ::close(fd);

    auto w2 = PendingLogWriter::Open(wal_path_, 16, 5);
    EXPECT_FALSE(w2.ok());
    EXPECT_NE(w2.status().message().find("CX_ERR_PWL_CORRUPTED"), std::string::npos);
}

// Opening a path under a non-existent directory fails at ::open (Open error branch).
TEST_F(PendingLogWriterTest, OpenUncreatablePathFails) {
    auto w = PendingLogWriter::Open("/nonexistent_dir_pwl_42/pending.wal", 16, 5);
    EXPECT_FALSE(w.ok());
    EXPECT_NE(w.status().message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);
}

// --- Branch coverage: injected append failures (SetAppendFailures). ----------

// One injected fault makes the next Append fail-fast without writing; the
// following Append succeeds (the fault counter drains to zero).
TEST_F(PendingLogWriterTest, InjectedAppendFailureThenRecovers) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto* pwl = w.value().get();

    pwl->SetAppendFailures(1);
    Status bad = pwl->Append(Pending(1, "d1", {1}));
    EXPECT_FALSE(bad.ok());
    EXPECT_NE(bad.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);

    // Counter drained → the next append lands normally.
    ASSERT_TRUE(pwl->Append(Pending(2, "d2", {2})).ok());
    auto all = pwl->ReadAll();
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all->size(), 1u);
    EXPECT_EQ((*all)[0].txn_id, 2u);
}

// AppendBatch drains every future even when faults are injected, surfacing the
// first error while still consuming the rest.
TEST_F(PendingLogWriterTest, InjectedAppendBatchSurfacesFirstError) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto* pwl = w.value().get();

    pwl->SetAppendFailures(5);  // more faults than entries → all fail
    std::vector<PendingEntry> batch;
    for (BlockId i = 1; i <= 3; ++i) batch.push_back(Pending(i, "d", {i}));
    Status s = pwl->AppendBatch(batch);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);

    pwl->SetAppendFailures(0);  // drain remaining faults for a clean teardown
}

// --- Branch coverage: Compact / GetStats over a freshly opened empty file. ----

// Compact on a header-only file is a no-op that still reopens cleanly, then
// appends work against the reopened fd.
TEST_F(PendingLogWriterTest, CompactEmptyFileIsNoOp) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto* pwl = w.value().get();

    ASSERT_TRUE(pwl->Compact().ok());
    EXPECT_EQ(pwl->SizeBytes(), PendingLogWriter::kHeaderSize);

    ASSERT_TRUE(pwl->Append(Pending(1, "d", {1})).ok());
    auto all = pwl->ReadAll();
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all->size(), 1u);
}

// GetStats over an empty file reports all-zero counts (the loop bodies are
// skipped; this drives the "no entries" path through both finalize loops).
TEST_F(PendingLogWriterTest, StatsOnEmptyFileAllZero) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto s = w.value()->GetStats();
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s->pending, 0u);
    EXPECT_EQ(s->committed, 0u);
    EXPECT_EQ(s->rollback, 0u);
}

// Compact's atomic-replace fails to open its temp file when the parent directory
// is read-only (open tmp error branch). The original file is left intact.
TEST_F(PendingLogWriterTest, CompactTempOpenFailsOnReadOnlyDir) {
    auto w = PendingLogWriter::Open(wal_path_, 16, 5);
    ASSERT_TRUE(w.ok());
    auto* pwl = w.value().get();
    ASSERT_TRUE(pwl->Append(Pending(1, "d", {1})).ok());

    // Make the directory read-only so creating "<path>.compact.tmp" fails.
    ASSERT_EQ(::chmod(dir_.c_str(), 0500), 0);
    Status s = pwl->Compact();
    ::chmod(dir_.c_str(), 0700);  // restore so TearDown can clean up
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);

    // The original PENDING record is still readable (compaction did not mutate it).
    auto all = pwl->ReadAll();
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all->size(), 1u);
}

TEST_F(PendingLogWriterTest, ConcurrentAppendsAllDurable) {
    auto w = PendingLogWriter::Open(wal_path_, /*max_batch=*/32, /*timeout_ms=*/5);
    ASSERT_TRUE(w.ok());
    auto* pwl = w.value().get();

    // Many threads append concurrently; group commit coalesces them. Every
    // record must end up durable exactly once.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                uint64_t txn = static_cast<uint64_t>(t) * 1000 + i;
                ASSERT_TRUE(pwl->Append(Pending(txn, "c", {static_cast<BlockId>(txn)})).ok());
            }
        });
    }
    for (auto& th : ts) th.join();

    auto all = pwl->ReadAll();
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all->size(), static_cast<size_t>(kThreads * kPerThread));
}

}  // namespace
}  // namespace cortrix::store
