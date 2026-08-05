// Error-path coverage for the index P-HNSW durability codes (WAL / snapshot) and the
// Semantic score scoring-store code. Each CX_ERR_* below had ZERO referencing test. We assert
// the *code string* surfaces on the returned cortrix::Status (the project carries the
// stable identity as a "<CX_ERR_*>: detail" message prefix per CODING_CONVENTIONS §3
// — there is no separate code field at the store layer), not merely that the call
// failed. The existing fault-sweep tests reach these I/O sites but assert only
// !ok(); these tests pin the precise CX_ERR_PHNSW_* / CX_ERR_SCORING_STORE identity.
//
// DEFERRED (documented at end of file): CX_ERR_PHNSW_INDEX_FULL and
// CX_ERR_PHNSW_RECOVERY_FAILED are dead constants — defined/exported but returned by
// no code path. See the DEFERRED block below.

#include "cortrix/store/phnsw/wal_writer.h"
#include "cortrix/store/phnsw/snapshot_manager.h"
#include "cortrix/store/phnsw_errors.h"
#include "cortrix/scoring/scoring_store.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "cortrix/store/phnsw/wal_entry.h"

#ifdef CORTRIX_ENABLE_FAULT_INJECT
#include "fault_inject/fault_inject.h"
#endif

namespace cortrix::store {
namespace {

namespace fs = std::filesystem;

// Substring helper: assert the stable CX_ERR_* identity is carried on the message.
::testing::AssertionResult HasCode(const Status& s, const char* code) {
    if (s.ok()) return ::testing::AssertionFailure() << "Status is ok(), expected " << code;
    if (s.message().find(code) == std::string::npos) {
        return ::testing::AssertionFailure()
               << "message '" << s.message() << "' does not contain " << code;
    }
    return ::testing::AssertionSuccess();
}

class PhnswErrPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("cortrix_errpath_phnsw_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }
    fs::path dir_;
};

// ---------------------------------------------------------------------------
// CX_ERR_PHNSW_WAL_WRITE_FAILED — ordinary input (no fault injection).
// WalWriter::Open does ::open(O_RDWR|O_CREAT); pointing at a path whose PARENT
// directory does not exist makes ::open return -1 (ENOENT) -> kWalWriteFailed.
// ---------------------------------------------------------------------------
TEST_F(PhnswErrPathTest, WalOpenOnMissingParentDirReturnsWalWriteFailed) {
    const std::string bad = (dir_ / "no_such_subdir" / "hnsw.wal").string();
    auto w = WalWriter::Open(bad, /*dim=*/8);
    ASSERT_FALSE(w.ok());
    EXPECT_TRUE(HasCode(w.status(), phnsw_errors::kWalWriteFailed));
}

// ---------------------------------------------------------------------------
// CX_ERR_PHNSW_SNAPSHOT_FAILED — ordinary input. SnapshotManager::Save rejects a
// null index before touching the filesystem (snapshot_manager.cpp:154).
// ---------------------------------------------------------------------------
TEST_F(PhnswErrPathTest, SnapshotSaveNullIndexReturnsSnapshotFailed) {
    SnapshotManager mgr(dir_.string(), /*keep_count=*/2);
    Status s = mgr.Save(/*index=*/nullptr, /*lsn=*/1, /*dim=*/8, /*M=*/16);
    EXPECT_TRUE(HasCode(s, phnsw_errors::kSnapshotFailed));
}

// ---------------------------------------------------------------------------
// CX_ERR_PHNSW_WAL_SYNC_FAILED — needs syscall fault injection. On a fresh WAL,
// WalWriter::Open writes + DurableSync()es a header (wal_writer.cpp:151). Arming a
// fault on the sync syscall BEFORE Open (the pwrite of the header succeeds first)
// drives the sync-failure branch deterministically. macOS uses F_FULLFSYNC
// ("fullfsync"); Linux uses fdatasync/fsync — try the platform set and require one
// to surface the code. Only compiled when the fault-inject seam is linked.
// ---------------------------------------------------------------------------
#ifdef CORTRIX_ENABLE_FAULT_INJECT
TEST_F(PhnswErrPathTest, WalOpenSyncFaultReturnsWalSyncFailed) {
    namespace fi = cortrix::fi;
    bool surfaced = false;
    for (const char* sync_op : {"fullfsync", "fdatasync", "fsync"}) {
        const std::string path = (dir_ / "hnsw.wal").string();
        ::unlink(path.c_str());
        fi::FailOp(sync_op, /*skip=*/0, /*count=*/1, EIO, "hnsw.wal");
        auto w = WalWriter::Open(path, /*dim=*/8);
        const bool consumed = fi::ConsumedCount() > 0;
        fi::Disarm();
        if (consumed) {
            // The header sync was the syscall we failed -> Open must surface the code.
            ASSERT_FALSE(w.ok()) << sync_op << ": sync fault did not surface a failure";
            EXPECT_TRUE(HasCode(w.status(), phnsw_errors::kWalSyncFailed)) << "op=" << sync_op;
            surfaced = true;
            break;
        }
        // This op is not the platform's durable-sync primitive; try the next.
    }
    EXPECT_TRUE(surfaced) << "no durable-sync syscall was exercised by WalWriter::Open";
}
#endif  // CORTRIX_ENABLE_FAULT_INJECT

// ---------------------------------------------------------------------------
// CX_ERR_SCORING_STORE — ordinary input. WriteScore prepares an UPDATE against the
// `blocks` table; opening an in-memory DB WITHOUT that table makes
// sqlite3_prepare_v2 fail ("no such table: blocks") -> CX_ERR_SCORING_STORE.
// (The existing test_scoring_store.cpp only covers the happy path + null-db
// InvalidArgument; the prepare/step failure code was untested.)
// ---------------------------------------------------------------------------
TEST(ScoringStoreErrPathTest, MissingBlocksTableReturnsScoringStore) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    // No `blocks` table created -> prepare of the UPDATE fails.
    Status s = scoring::WriteScore(db, /*block_id=*/1, /*semantic_score=*/0.6f);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_SCORING_STORE"), std::string::npos)
        << "message: " << s.message();
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    sqlite3_close(db);
}

// ===========================================================================
// DEFERRED — no hollow tests written:
//
//   CX_ERR_PHNSW_INDEX_FULL    (phnsw_errors::kIndexFull)
//     Defined and exported in phnsw_errors.h but returned by NO code path. The
//     capacity-exceeded case in phnsw.cpp AddPoint throws std::exception (caught and
//     surfaced differently), never mapping to kIndexFull. Triggering it would require
//     a SOURCE change to actually return the code (e.g. phnsw.cpp AddPoint when
//     cur_element_count >= max_elements). Reported as a dead-constant bug.
//
//   CX_ERR_PHNSW_RECOVERY_FAILED  (phnsw_errors::kRecoveryFailed)
//     Defined/exported but returned by NO code path; only referenced in a comment in
//     iindex_factory.h. The corrupt-snapshot load path returns
//     CX_ERR_PHNSW_SNAPSHOT_CORRUPTED (snapshot_manager.cpp), not RECOVERY_FAILED.
//     (The similarly named CX_ERR_PWL_RECOVERY_FAILED in write_coordinator.cpp is a
//     DIFFERENT code — the write coordinator PWL layer — out of this assignment's scope.) Reported
//     as a dead-constant bug.
// ===========================================================================

}  // namespace
}  // namespace cortrix::store
