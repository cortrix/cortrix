// R7 cov-store — targeted branch-coverage supplements for the F25/F01
// persistence layer. These exercise specific error/edge arms the existing
// suites leave uncovered:
//
//   A1  GroupCommitWriter: a *throwing* IWalSink (the R2-M3 defensive catch-all
//       around FlushBatch / FlushLoop + ResolvePendingWaiters). The existing
//       RecordingWalSink only returns error Statuses, never throws, so the two
//       catch arms + the last-resort waiter resolver sat uncovered.
//   A3  WriteCoordinator::Commit: when every retry fails, the doc_id is still
//       released from active_docs_ (R2-M6) so the same doc is writable again.
//       The existing CommitFailsWhenRetriesExhausted asserts the error but not
//       the no-leak property — this adds that regression assertion.
//   A4  PendingLogWriter::ReadAll: the corrupt-tail repair path's ftruncate
//       FAILURE arm (returns CX_ERR_PWL_WRITE_FAILED). Existing corrupt-tail
//       tests all let ftruncate succeed; the fault-inject seam reaches the
//       error arm.  (The DurableSync after ftruncate is intentionally
//       result-ignored in the source, so there is no sync-failure arm to cover.)
//   A5  SnapshotManager::Load: an orphan .index with no committed .meta →
//       any_seen stays false → returns Ok with a null index (caller starts
//       empty). Distinct from the "all snapshots corrupt" (any_seen==true)
//       arm the existing tests cover.
//   A2  CortrixBlobLocal::load: the mid-read failure arm (returns -1). Attempted
//       via the fault-inject `read` seam; std::ifstream buffering may not route a
//       single byte through an interposable read, so this is a best-effort sweep
//       — if no read is intercepted it asserts nothing failed and is documented
//       as physically-unreachable-from-this-seam in the QA list (no hacks).
//
// Physically-unreachable arms (NOT force-tested — F23 §4.1.bis #4) are listed in
// the task's QA unreachable-branch report, not here.
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

#include <hnswlib/hnswlib.h>  // complete HierarchicalNSW for the unique_ptr<> in A5

#include "cortrix/store/cortrix_blob_local.h"
#include "cortrix/store/group_commit_writer.h"
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/pending_entry.h"
#include "cortrix/store/pending_log_writer.h"
#include "cortrix/store/phnsw/snapshot_manager.h"
#include "cortrix/store/recover_stores.h"
#include "cortrix/store/write_coordinator.h"

// A2/A4 drive the syscall fault-injection seam (read/ftruncate), which links the
// macOS-only dyld-interpose library (tests/CMakeLists.txt CORTRIX_ENABLE_FAULT_INJECT
// block — OFF on Linux/ASAN). Guard those two tests behind the same define so this
// file still BUILDS and runs A1/A3/A5 everywhere (Linux CI included); A2/A4 compile
// only where the seam is linked. The build system defines CORTRIX_ENABLE_FAULT_INJECT
// on this target when the seam is on (see report to lead for the one-line CMake add).
#ifdef CORTRIX_ENABLE_FAULT_INJECT
#include "fault_inject/fault_inject.h"
#endif

namespace cortrix::store {
namespace {

#ifdef CORTRIX_ENABLE_FAULT_INJECT
namespace fi = cortrix::fi;

// RAII: always Disarm at scope exit so a failing EXPECT never leaks an armed
// fault into the next test (mirrors test_syscall_fault_inject.cpp).
struct ArmGuard {
    ~ArmGuard() { fi::Disarm(); }
};
#endif

// ===================================================================
// A1 — GroupCommitWriter: throwing IWalSink (R2-M3 defensive catch-all)
// ===================================================================

// A sink whose WriteBatch and/or Sync throw, to drive the catch(std::exception)
// / catch(...) arms in FlushBatch (and, on a non-FlushBatch throw, the outer
// FlushLoop guard + ResolvePendingWaiters). The contract under test: every
// Submit() future is still resolved with a non-ok Status (never broken_promise),
// and the flush thread keeps running (no std::terminate).
class ThrowingWalSink : public IWalSink {
public:
    enum class Mode { kThrowStdOnWrite, kThrowStdOnSync, kThrowNonStdOnWrite };

    explicit ThrowingWalSink(Mode m) : mode_(m) {}

    Status WriteBatch(const std::vector<std::vector<uint8_t>>&) override {
        if (mode_ == Mode::kThrowStdOnWrite) {
            throw std::runtime_error("injected WriteBatch throw");
        }
        if (mode_ == Mode::kThrowNonStdOnWrite) {
            throw 42;  // non-std exception → catch(...) arm
        }
        return Status::Ok();  // kThrowStdOnSync: write succeeds, sync throws
    }
    Status Sync() override {
        if (mode_ == Mode::kThrowStdOnSync) {
            throw std::runtime_error("injected Sync throw");
        }
        return Status::Ok();
    }

private:
    Mode mode_;
};

TEST(GroupCommitThrowingSinkTest, WriteThrowStdResolvesWaiterNotOk) {
    ThrowingWalSink sink(ThrowingWalSink::Mode::kThrowStdOnWrite);
    GroupCommitWriter w(&sink, /*max_batch=*/4, /*timeout_ms=*/10);

    auto fut = w.Submit({1, 2, 3});
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "a throwing WriteBatch must not leave the future hanging";
    EXPECT_FALSE(fut.get().ok()) << "throwing sink → batch reported not durable";
}

TEST(GroupCommitThrowingSinkTest, SyncThrowStdResolvesWaiterNotOk) {
    ThrowingWalSink sink(ThrowingWalSink::Mode::kThrowStdOnSync);
    GroupCommitWriter w(&sink, /*max_batch=*/4, /*timeout_ms=*/10);

    auto fut = w.Submit({7});
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(fut.get().ok()) << "a throwing Sync must surface as not-ok";
}

TEST(GroupCommitThrowingSinkTest, NonStdThrowResolvesWaiterNotOk) {
    ThrowingWalSink sink(ThrowingWalSink::Mode::kThrowNonStdOnWrite);
    GroupCommitWriter w(&sink, /*max_batch=*/4, /*timeout_ms=*/10);

    auto fut = w.Submit({9});
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "a non-std throw must hit the catch(...) arm, not std::terminate";
    EXPECT_FALSE(fut.get().ok());
}

TEST(GroupCommitThrowingSinkTest, FlushThreadSurvivesThrowAndKeepsDraining) {
    // After a throwing flush the background loop must keep running: a second
    // Submit on the same writer is still resolved (covers "loop continues").
    ThrowingWalSink sink(ThrowingWalSink::Mode::kThrowStdOnWrite);
    GroupCommitWriter w(&sink, /*max_batch=*/1, /*timeout_ms=*/5);

    auto f1 = w.Submit({1});
    ASSERT_EQ(f1.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(f1.get().ok());

    auto f2 = w.Submit({2});
    ASSERT_EQ(f2.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "the flush thread must survive the first throw and resolve later work";
    EXPECT_FALSE(f2.get().ok());
}

// ===================================================================
// A3 — WriteCoordinator: Commit retry-exhausted releases the doc_id
// ===================================================================

TEST(WriteCoordinatorCommitLeakTest, RetryExhaustedReleasesDocIdNoLeak) {
    char tmpl[] = "/tmp/cortrix_wc_r7_XXXXXX";
    char* d = ::mkdtemp(tmpl);
    ASSERT_NE(d, nullptr);
    const std::string dir = d;

    WriteCoordinatorConfig cfg;
    cfg.commit_max_retries = 1;     // 2 attempts total — keep the test fast
    cfg.commit_retry_base_ms = 1;
    WriteCoordinator wc(dir, cfg, nullptr, nullptr, nullptr);
    ASSERT_TRUE(wc.Init().ok());

    TxnHandle t;
    ASSERT_TRUE(wc.BeginWrite("doc-leak", {1}, /*writes_blob=*/true, &t).ok());

    // Exhaust the retry budget so Commit fails after every attempt.
    wc.SetWalAppendFailuresForTest(10);
    Status s = wc.Commit(t);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);
    wc.SetWalAppendFailuresForTest(0);

    // R2-M6 regression: despite the failed commit, the doc_id was released from
    // active_docs_ (the three-way stores hold the data; recovery infers
    // COMMITTED). So a fresh write for the SAME doc_id must be accepted — if the
    // doc_id leaked, BeginWrite would reject it with DOC_WRITE_IN_PROGRESS.
    TxnHandle t2;
    EXPECT_TRUE(wc.BeginWrite("doc-leak", {2}, /*writes_blob=*/true, &t2).ok())
        << "doc_id must be writable again after a retry-exhausted commit (no leak)";

    // cleanup
    ::unlink((dir + "/pending.wal").c_str());
    ::unlink((dir + "/pending.wal.compact.tmp").c_str());
    ::rmdir(dir.c_str());
}

// ===================================================================
// A4 — PendingLogWriter::ReadAll corrupt-tail repair: ftruncate FAILS
// (needs the syscall fault seam — macOS-only, see top-of-file note)
// ===================================================================
#ifdef CORTRIX_ENABLE_FAULT_INJECT

TEST(PendingLogReadAllRepairFaultTest, FtruncateFailureOnTailRepairSurfaces) {
    char tmpl[] = "/tmp/cortrix_pwl_r7_XXXXXX";
    char* d = ::mkdtemp(tmpl);
    ASSERT_NE(d, nullptr);
    const std::string dir = d;
    const std::string path = dir + "/pending.wal";

    // Lay down two good records, then append a garbage tail so ReadAll's repair
    // path (ftruncate-to-last-valid) runs.
    {
        auto opened = PendingLogWriter::Open(path, /*max_batch=*/4, /*timeout_ms=*/5);
        ASSERT_TRUE(opened.ok());
        auto& w = *opened.value();
        for (uint64_t t = 1; t <= 2; ++t) {
            PendingEntry e;
            e.state = PendingEntry::State::kPending;
            e.txn_id = t;
            e.doc_id = "d" + std::to_string(t);
            e.block_ids = {static_cast<BlockId>(t)};
            ASSERT_TRUE(w.Append(e).ok());
        }
        w.Shutdown();
    }
    {
        int fd = ::open(path.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        off_t end = ::lseek(fd, 0, SEEK_END);
        const uint8_t junk[9] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05};
        ASSERT_EQ(::pwrite(fd, junk, sizeof(junk), end), static_cast<ssize_t>(sizeof(junk)));
        ::close(fd);
    }

    // Arm the ftruncate fault BEFORE reopening. Critical: the seam's fd->path
    // table is populated by its interposed open() ONLY while armed (g_active) —
    // an open() done before FailOp() is a pure pass-through that records no fd,
    // so a later fd-based ftruncate would never path-match. Arming ftruncate does
    // NOT disturb Open (Open does open()+pread(), no ftruncate), but it lets
    // my_open record this fd so ReadAll's ftruncate (the only ftruncate here)
    // hits the armed, path-matched fd. The arm window must span Open + ReadAll.
    bool surfaced = false;
    {
        ArmGuard g;
        fi::FailOp("ftruncate", /*skip=*/0, /*count=*/1, EIO, "pending.wal");

        auto reopened = PendingLogWriter::Open(path, 4, 5);  // open() recorded (armed)
        ASSERT_TRUE(reopened.ok());
        auto& w = *reopened.value();

        // ReadAll scans, finds the corrupt tail, and ftruncate's to the last valid
        // record — that ftruncate is failed by the seam. ReadAll must surface
        // CX_ERR_PWL_WRITE_FAILED, not silently swallow the trim failure.
        Status s = w.ReadAll().status();
        const int consumed = fi::ConsumedCount();
        if (consumed > 0) {
            surfaced = !s.ok();
            EXPECT_FALSE(s.ok())
                << "ftruncate failure during tail repair must surface as not-ok";
            if (!s.ok()) {
                EXPECT_NE(s.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);
            }
        }
        w.Shutdown();
        fi::Disarm();
    }
    // The ftruncate call site is reached only when a corrupt tail exists, which
    // we arranged; require the seam actually fired (else the test proves nothing).
    EXPECT_TRUE(surfaced) << "ftruncate seam did not fire on the tail-repair path";

    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

#endif  // CORTRIX_ENABLE_FAULT_INJECT (A4)

// ===================================================================
// A5 — SnapshotManager::Load orphan .index (no .meta) → Ok + null index
// ===================================================================

TEST(SnapshotManagerOrphanTest, OrphanIndexWithoutMetaTreatedAsNoSnapshot) {
    char tmpl[] = "/tmp/cortrix_snap_r7_XXXXXX";
    char* d = ::mkdtemp(tmpl);
    ASSERT_NE(d, nullptr);
    const std::string dir = d;

    // Drop a well-named snapshot .index but NO sibling .meta. ListSnapshots finds
    // it, but the per-file loop skips it (no/short meta) → any_seen stays false →
    // Load returns Ok with a null index (the "only orphan .index files" branch).
    const std::string orphan = dir + "/hnsw_123.index";
    {
        int fd = ::open(orphan.c_str(), O_RDWR | O_CREAT, 0644);
        ASSERT_GE(fd, 0);
        const char body[16] = {0};  // any bytes; never read without a .meta
        ASSERT_EQ(::pwrite(fd, body, sizeof(body), 0), static_cast<ssize_t>(sizeof(body)));
        ::close(fd);
    }

    SnapshotManager mgr(dir, /*keep_count=*/2);
    // Load needs an L2Space pointer only when it actually constructs an index;
    // here it never does (orphan skipped), so a null space is never dereferenced.
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    uint64_t lsn = 12345;  // sentinel; must be reset to 0 on the no-snapshot path
    Status s = mgr.Load(/*space=*/nullptr, /*max_elements=*/1000, &loaded, &lsn);

    EXPECT_TRUE(s.ok()) << "an orphan .index with no .meta is not an error: " << s.message();
    EXPECT_EQ(loaded, nullptr) << "no committed snapshot → caller starts empty";
    EXPECT_EQ(lsn, 0u) << "out_lsn reset to 0 on the no-snapshot path";

    ::unlink(orphan.c_str());
    ::rmdir(dir.c_str());
}

// ===================================================================
// A2 — CortrixBlobLocal::load mid-read failure (best-effort via read seam)
// (needs the syscall fault seam — macOS-only, see top-of-file note)
// ===================================================================
#ifdef CORTRIX_ENABLE_FAULT_INJECT

TEST(BlobLoadReadFaultTest, MidReadFailureReportedOrSeamUnreachable) {
    char tmpl[] = "/tmp/cortrix_blob_r7_XXXXXX";
    char* d = ::mkdtemp(tmpl);
    ASSERT_NE(d, nullptr);
    const std::string dir = d;
    // Path-filter substring: unique mkdtemp basename (see test_syscall_fault_inject
    // — /tmp is a symlink to /private/tmp on macOS, so filter by basename).
    auto slash = dir.rfind('/');
    const std::string dir_tag = (slash == std::string::npos) ? dir : dir.substr(slash + 1);

    CortrixBlobLocal blob(dir);
    const std::string ns = "rd";
    const std::string doc_id = "01JTESTDOC0000000000000777";
    // Store enough bytes that the load issues a real read of the body.
    std::vector<uint8_t> payload(4096, 0xAB);
    ASSERT_EQ(blob.store(ns, doc_id, payload.data(), payload.size()), 0);

    // Sweep a read fault across the load. ifstream buffering may or may not route
    // the body read through an interposable ::read; if the seam never fires we do
    // NOT contrive a failure (no hacks) — the arm is then documented unreachable
    // from this seam in the QA list.
    bool seam_fired = false;
    for (int k = 0; k < 8; ++k) {
        ArmGuard g;
        fi::FailOp("read", /*skip=*/k, /*count=*/1, EIO, dir_tag.c_str());
        std::vector<uint8_t> out;
        int rc = blob.load(ns, doc_id, out);
        const int matched = fi::MatchedCount();
        const int consumed = fi::ConsumedCount();
        fi::Disarm();
        if (consumed > 0) {
            seam_fired = true;
            // A consumed read fault on the body must surface as the -1 read-error
            // path (NOT -2 not-found, NOT 0 success).
            EXPECT_EQ(rc, -1)
                << "read #" << k << " injected but load() did not report a read error";
        }
        if (matched <= k) break;  // swept past the last read call site
    }

    if (!seam_fired) {
        GTEST_SKIP() << "blob load() issues no interposable ::read (ifstream buffering); "
                        "mid-read -1 arm is unreachable from the fault seam — see QA "
                        "unreachable-branch list";
    }

    std::error_code ec;
    ::unlink((dir + "/ns_" + ns).c_str());  // best-effort; recursive cleanup below
    (void)ec;
    std::string cmd = "rm -rf '" + dir + "'";
    (void)::system(cmd.c_str());
}

#endif  // CORTRIX_ENABLE_FAULT_INJECT (A2)

}  // namespace
}  // namespace cortrix::store
