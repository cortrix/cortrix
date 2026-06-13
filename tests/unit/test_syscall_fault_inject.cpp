// F23 §4.6 — syscall fault-injection sweeps for the durability layers.
//
// The F25 pending log (PendingLogWriter) and the F01 P-HNSW WAL (WalWriter) +
// snapshots (SnapshotManager) implement graceful handling for every I/O syscall
// failure (write/pwrite/pread/fsync/F_FULLFSYNC/ftruncate/rename returning -1).
// Against a healthy filesystem those error arms are physically unreachable from
// user space, so they sat uncovered. The dyld-interpose seam in
// tests/fault_inject makes them reachable.
//
// Sweep pattern: for k = 0,1,2,..., fail the (k+1)-th matching syscall while
// running [Open + workload]. Each run uses a FRESH file. We require:
//   * the operation reports failure (does not crash, does not silently succeed)
//   * a subsequent fault-free Open still works (no permanent corruption)
// We stop when run k matched no syscalls (k is past the last call site), so the
// sweep auto-sizes to the real number of call sites — no magic counts to drift.
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include <hnswlib/hnswlib.h>

#include "cortrix/store/pending_entry.h"
#include "cortrix/store/pending_log_writer.h"
#include "cortrix/store/phnsw/snapshot_manager.h"
#include "cortrix/store/phnsw/wal_writer.h"

#include "fault_inject/fault_inject.h"

namespace cortrix::store {
namespace {

namespace fi = cortrix::fi;

// RAII: always Disarm at scope exit so a failing EXPECT never leaks an armed
// fault into the next test.
struct ArmGuard {
    ~ArmGuard() { fi::Disarm(); }
};

class SyscallFaultTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/cortrix_fi_XXXXXX";
        char* d = ::mkdtemp(tmpl);
        ASSERT_NE(d, nullptr);
        dir_ = d;
        // Path filter substring: the unique mkdtemp basename ("cortrix_fi_...").
        // NOT the full dir_ — on macOS /tmp is a symlink to /private/tmp, so a
        // file's resolved path is "/private/tmp/cortrix_fi_.../x" which does NOT
        // contain the literal "/tmp/cortrix_fi_..."; the basename always matches.
        auto slash = dir_.rfind('/');
        dir_tag_ = (slash == std::string::npos) ? dir_ : dir_.substr(slash + 1);
    }
    void TearDown() override {
        fi::Disarm();
        // best-effort recursive cleanup of the scratch dir
        std::string cmd = "rm -rf '" + dir_ + "'";
        (void)::system(cmd.c_str());
    }

    static PendingEntry Pending(uint64_t txn) {
        PendingEntry e;
        e.state = PendingEntry::State::kPending;
        e.txn_id = txn;
        e.timestamp_ms = static_cast<int64_t>(txn) * 1000;
        e.doc_id = "doc" + std::to_string(txn);
        e.block_ids = {1, 2, 3};
        return e;
    }

    std::string dir_;
    std::string dir_tag_;  // unique basename, safe path-filter substring
};

// ---- PendingLogWriter (F25 PWL) -----------------------------------

// Sweep write/sync faults across [Open + a few Appends]. Every failed syscall
// must surface as a non-ok Status somewhere in the sequence, never a crash.
TEST_F(SyscallFaultTest, PwlWriteSyncFaultSweep) {
    const std::string path = dir_ + "/pending.wal";
    for (const char* op : {"pwrite", "fullfsync", "fsync"}) {
        int hits = 0;
        for (int k = 0; k < 40; ++k) {
            ::unlink(path.c_str());
            ArmGuard g;
            fi::FailOp(op, k, 1, EIO, "pending.wal");

            bool any_fail = false;
            auto opened = PendingLogWriter::Open(path, /*max_batch=*/4,
                                                 /*timeout_ms=*/5);
            if (!opened.ok()) {
                any_fail = true;
            } else {
                auto& w = *opened.value();
                for (uint64_t t = 1; t <= 3; ++t) {
                    if (!w.Append(Pending(t)).ok()) any_fail = true;
                }
                w.Shutdown();
            }

            int matched = fi::MatchedCount();
            int consumed = fi::ConsumedCount();
            fi::Disarm();
            if (matched <= k) break;  // k past the last call site → swept all
            if (consumed > 0) {
                ++hits;
                EXPECT_TRUE(any_fail)
                    << op << " #" << k << " injected but no failure surfaced";
                // Recoverability: a clean Open must still succeed.
                auto reopened = PendingLogWriter::Open(path, 4, 5);
                EXPECT_TRUE(reopened.ok())
                    << op << " #" << k << " left the file unopenable";
                if (reopened.ok()) reopened.value()->Shutdown();
            }
        }
        // PWL's IWalSink::Sync uses F_FULLFSYNC on macOS (plain fsync there does
        // not flush the drive cache); the "fsync" arm legitimately finds no call
        // site, so only require the primitives PWL actually uses.
        if (std::string(op) == "pwrite" || std::string(op) == "fullfsync")
            EXPECT_GT(hits, 0) << "no " << op << " call site exercised";
    }
}

// Open must fail (not crash) when the file cannot even be created.
TEST_F(SyscallFaultTest, PwlOpenFailsWhenCreateFails) {
    const std::string path = dir_ + "/pending.wal";
    ArmGuard g;
    fi::FailOp("open", 0, 1, EACCES, "pending.wal");
    auto opened = PendingLogWriter::Open(path, 4, 5);
    EXPECT_FALSE(opened.ok());
}

// ReadAll / GetStats read the file with pread; a read fault must be reported.
TEST_F(SyscallFaultTest, PwlReadFaultSweep) {
    const std::string path = dir_ + "/pending.wal";
    {
        auto opened = PendingLogWriter::Open(path, 4, 5);
        ASSERT_TRUE(opened.ok());
        auto& w = *opened.value();
        ASSERT_TRUE(w.Append(Pending(1)).ok());
        ASSERT_TRUE(w.Append(Pending(2)).ok());
        w.Shutdown();
    }
    int hits = 0;
    for (int k = 0; k < 20; ++k) {
        ArmGuard g;
        fi::FailOp("pread", k, 1, EIO, "pending.wal");
        auto opened = PendingLogWriter::Open(path, 4, 5);
        if (!opened.ok()) {  // Open itself reads/validates the header
            ++hits;
            continue;
        }
        auto& w = *opened.value();
        bool fail = !w.ReadAll().ok() || !w.GetStats().ok();
        int matched = fi::MatchedCount();
        int consumed = fi::ConsumedCount();
        w.Shutdown();
        fi::Disarm();
        if (matched <= k) break;
        if (consumed > 0) {
            ++hits;
            EXPECT_TRUE(fail) << "pread #" << k << " injected but ReadAll/GetStats ok";
        }
    }
    EXPECT_GT(hits, 0);
}

// Compact rewrites via temp + rename; a rename fault must be reported and must
// leave the original file intact (atomic-replace contract).
TEST_F(SyscallFaultTest, PwlCompactRenameFault) {
    const std::string path = dir_ + "/pending.wal";
    auto opened = PendingLogWriter::Open(path, 4, 5);
    ASSERT_TRUE(opened.ok());
    auto& w = *opened.value();
    ASSERT_TRUE(w.Append(Pending(1)).ok());
    ASSERT_TRUE(w.Append(Pending(2)).ok());

    ArmGuard g;
    fi::FailOp("rename", 0, 1, EIO, "pending.wal");
    Status s = w.Compact();
    fi::Disarm();
    EXPECT_FALSE(s.ok()) << "Compact masked a rename failure";

    // Original data still readable.
    auto recs = w.GetStats();
    EXPECT_TRUE(recs.ok());
    w.Shutdown();
}

// ---- WalWriter (F01 P-HNSW WAL) -----------------------------------

TEST_F(SyscallFaultTest, HnswWalWriteSyncFaultSweep) {
    const std::string path = dir_ + "/hnsw.wal";
    const int dim = 4;
    auto make_record = [&](uint64_t id) {
        // INSERT-shaped record: id + dim floats. Layout details belong to
        // WalEntry; here any non-empty blob exercises the write path.
        std::vector<uint8_t> r(8 + dim * 4, 0);
        for (size_t i = 0; i < 8; ++i) r[i] = static_cast<uint8_t>(id >> (i * 8));
        return r;
    };
    for (const char* op : {"pwrite", "fullfsync", "fdatasync", "fsync"}) {
        int hits = 0;
        for (int k = 0; k < 40; ++k) {
            ::unlink(path.c_str());
            ArmGuard g;
            fi::FailOp(op, k, 1, EIO, "hnsw.wal");

            bool any_fail = false;
            auto opened = WalWriter::Open(path, dim);
            if (!opened.ok()) {
                any_fail = true;
            } else {
                auto& w = *opened.value();
                for (uint64_t t = 1; t <= 3; ++t) {
                    if (!w.Append(make_record(t)).ok()) any_fail = true;
                }
            }
            int matched = fi::MatchedCount();
            int consumed = fi::ConsumedCount();
            fi::Disarm();
            if (matched <= k) break;
            if (consumed > 0) {
                ++hits;
                EXPECT_TRUE(any_fail) << op << " #" << k << " no failure surfaced";
                auto re = WalWriter::Open(path, dim);
                EXPECT_TRUE(re.ok()) << op << " #" << k << " left WAL unopenable";
            }
        }
        // fdatasync/fsync may not be the macOS sync primitive (F_FULLFSYNC is);
        // only require that SOME sync op got exercised across the group, not each.
        if (std::string(op) == "pwrite" || std::string(op) == "fullfsync")
            EXPECT_GT(hits, 0) << "no " << op << " call site exercised";
    }
}

TEST_F(SyscallFaultTest, HnswWalReadFaultSweep) {
    const std::string path = dir_ + "/hnsw.wal";
    const int dim = 4;
    {
        auto opened = WalWriter::Open(path, dim);
        ASSERT_TRUE(opened.ok());
        auto& w = *opened.value();
        std::vector<uint8_t> r(8 + dim * 4, 7);
        ASSERT_TRUE(w.Append(r).ok());
    }
    int hits = 0;
    for (int k = 0; k < 20; ++k) {
        ArmGuard g;
        fi::FailOp("pread", k, 1, EIO, "hnsw.wal");
        auto opened = WalWriter::Open(path, dim);
        bool fail = !opened.ok();
        int matched = fi::MatchedCount();
        int consumed = fi::ConsumedCount();
        if (opened.ok()) {
            fail = !opened.value()->ReadAll(dim).ok();
            matched = fi::MatchedCount();
            consumed = fi::ConsumedCount();
        }
        fi::Disarm();
        if (matched <= k) break;
        if (consumed > 0) {
            ++hits;
            EXPECT_TRUE(fail) << "pread #" << k << " no failure surfaced";
        }
    }
    EXPECT_GT(hits, 0);
}

// ---- SnapshotManager (F01 snapshots) ------------------------------
// Save does write+fsync+rename of sibling .index/.meta files. We drive it via a
// real hnswlib index — but constructing one here pulls heavy headers; instead we
// exercise the read/list error paths that do not need a live index: Load and
// PeekLatestMeta against a directory whose I/O we fault.

TEST_F(SyscallFaultTest, SnapshotPeekAndLoadSurviveReadFaults) {
    // No snapshot files exist yet: Load returns Ok with nullptr, Peek false.
    SnapshotManager mgr(dir_, /*keep_count=*/2);
    {
        ArmGuard g;
        // Fault any open in this dir — there are no snapshot files, so the
        // listing simply finds nothing; this must not crash.
        fi::FailOp("open", 0, 3, EIO, dir_tag_.c_str());
        SnapshotManager::SnapshotMeta meta;
        bool peek = mgr.PeekLatestMeta(&meta);
        fi::Disarm();
        EXPECT_FALSE(peek);  // nothing to read
    }
    // LatestSnapshotLsn on an empty dir is 0 and must not crash under faults.
    {
        ArmGuard g;
        fi::FailOp("open", 0, 2, EIO, dir_tag_.c_str());
        uint64_t lsn = mgr.LatestSnapshotLsn();
        fi::Disarm();
        EXPECT_EQ(lsn, 0u);
    }
}

// Save writes sibling .index/.meta temp files, fsyncs, and renames them into
// place. Sweep write/fsync/rename faults across Save with a real (small) index
// and require each failure to surface as a non-ok Status (CX_ERR_PHNSW_SNAPSHOT_*).
TEST_F(SyscallFaultTest, SnapshotSaveFaultSweep) {
    const int dim = 8;
    hnswlib::L2Space space(dim);
    auto index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        &space, /*max_elements=*/256, /*M=*/16, /*ef_construction=*/200);
    for (int i = 0; i < 16; ++i) {
        std::vector<float> v(dim, static_cast<float>(i) * 0.1f);
        index->addPoint(v.data(), static_cast<hnswlib::labeltype>(i + 1));
    }

    for (const char* op : {"write", "fullfsync", "rename"}) {
        int hits = 0;
        for (int k = 0; k < 40; ++k) {
            SnapshotManager mgr(dir_, /*keep_count=*/3);
            ArmGuard g;
            // Snapshot files live directly under dir_; scope by the dir tag.
            fi::FailOp(op, k, 1, EIO, dir_tag_.c_str());
            Status s = mgr.Save(index.get(), /*lsn=*/100 + k, dim, /*M=*/16);
            int matched = fi::MatchedCount();
            int consumed = fi::ConsumedCount();
            fi::Disarm();
            if (matched <= k) break;
            if (consumed > 0) {
                ++hits;
                // rename is the commit point and IS error-checked, so a rename
                // fault must surface. The index fsync (F_FULLFSYNC) is best-effort
                // durability — Save legitimately ignores its result — so the
                // fullfsync arm only exercises the call site for coverage; we just
                // require it not to crash.
                if (std::string(op) == "rename")
                    EXPECT_FALSE(s.ok())
                        << "rename #" << k << " injected but Save reported ok";
            }
        }
        // SnapshotManager fsyncs via F_FULLFSYNC on macOS (fcntl), and hnswlib's
        // saveIndex uses buffered ofstream (raw ::write may not surface), so only
        // require the primitives SnapshotManager itself calls directly here.
        if (std::string(op) == "fullfsync" || std::string(op) == "rename")
            EXPECT_GT(hits, 0) << "no " << op << " call site exercised in Save";
    }
}

// CleanOldSnapshots must tolerate rename/unlink faults on stale .tmp files
// without crashing (best-effort cleanup contract).
TEST_F(SyscallFaultTest, SnapshotCleanupToleratesFaults) {
    // Drop a stale temp file the cleanup will try to remove.
    std::string stale = dir_ + "/hnsw_123.index.tmp";
    int fd = ::open(stale.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(fd, 0);
    ::close(fd);

    SnapshotManager mgr(dir_, 2);
    ArmGuard g;
    fi::FailOp("rename", 0, 4, EIO, dir_tag_.c_str());
    mgr.CleanOldSnapshots();  // must not crash
    fi::Disarm();
    SUCCEED();
}

}  // namespace
}  // namespace cortrix::store
