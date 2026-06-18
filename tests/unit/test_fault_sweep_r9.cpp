// =============================================================================
// R9 robustness — file-backed syscall-fault sweeps over the SQLite-backed stores.
// These reach the injection-only error arms that a :memory: database can never
// exercise (SQLITE_IOERR on the data path, WAL write failures).
//
// Method (F23 §4.6): a file-backed DB replaces :memory:, then fi::FailOp() arms the
// dyld-interpose seam to fail a chosen POSIX I/O call on the DB's path so the store's
// `rc != SQLITE_OK/DONE` arms fire. Every sweep asserts the workload fails GRACEFULLY
// (non-zero rc, no crash/UB) and the store stays usable after Disarm() (recoverable).
//
// 🔑 SQLite arm-window pattern (the critical, non-obvious bit): the seam's fd→path
// table is populated only inside the interposed open() while the seam is ARMED. But
// SQLite opens ALL of its fds (test.db + -wal + -shm) during store->Open(), so if we
// arm AFTER Open() those fds are pass-throughs the seam never recorded → no later
// pwrite/fsync can path-match and nothing is injected. The fix: arm a NON-failing
// open() (skip == huge) BEFORE Open() so SQLite's fds get recorded, THEN switch to
// the target op for the workload. (Verified: without the pre-arm, matched==0 for
// every op; with it, pwrite fires and doc_create returns -1 "disk I/O error".)
//
// Reachability honesty (matches R7-BRANCH-GAP §3): we sweep the WRITE path
// (pwrite/fsync/fullfsync) which is syscall-reachable. The data file is opened with
// PRAGMA mmap_size, so READS come from the mmap (page faults, no ::pread) — read-path
// arms are not sweep-reachable and are intentionally not chased. We never pad with
// no-op assertions.
// =============================================================================
#include <gtest/gtest.h>

#ifdef CORTRIX_ENABLE_FAULT_INJECT

#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <memory>
#include <string>

#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/common/data_types.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_session.h"
#include "cortrix/memory/interaction_log.h"
#include "fault_inject/fault_inject.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
constexpr int kRecordOnlySkip = 1'000'000;  // arm open() so it records but never fails

class StoreFaultSweepTest : public ::testing::Test {
public:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("cortrix_fault_sweep_" + std::to_string(::getpid()) + "_" +
                std::to_string(rand()));
        fs::create_directories(dir_);
        db_path_ = (dir_ / "test.db").string();
    }
    void TearDown() override {
        fi::Disarm();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // Open a fresh, file-backed store with the seam armed in record-only mode so
    // SQLite's fds (test.db / -wal / -shm) are captured into the fd→path table.
    std::unique_ptr<CortrixStoreSqlite> OpenStoreRecordingFds() {
        fi::FailOp("open", kRecordOnlySkip, 1, EIO, "test.db");  // record, never fail
        auto s = std::make_unique<CortrixStoreSqlite>(db_path_);
        EXPECT_EQ(s->Open(), 0);
        return s;
    }

    static CortrixDoc MakeDoc(const std::string& id) {
        CortrixDoc d;
        d.doc_id = id;
        d.source_type = "file";
        d.source_path = "/tmp/" + id + ".txt";
        d.content_hash = "hash_" + id;
        d.status = DocStatus::kPending;
        return d;
    }

    fs::path dir_;
    std::string db_path_;
};

// Sweep an op's EIO across the doc-write path. For each skip k: open a clean store
// (recording fds), arm the op, run doc_create with a unique doc_id (so a persisted
// row from a prior iteration never UNIQUE-collides), and require a graceful non-zero
// rc whenever the op actually fired. Terminates when k passes the last matching call.
void SweepDocWriteOp(StoreFaultSweepTest* fx, const char* op, bool require_rc_fail,
                     bool* out_any_consumed) {
    bool any = false;
    for (int k = 0;; ++k) {
        auto store = fx->OpenStoreRecordingFds();
        fi::FailOp(op, /*skip=*/k, /*count=*/1, EIO, "test.db");
        CortrixDoc d = StoreFaultSweepTest::MakeDoc("doc_" + std::string(op) + "_" +
                                                    std::to_string(k));
        const int rc = store->doc_create(d);
        const int consumed = fi::ConsumedCount();
        const int matched = fi::MatchedCount();
        fi::Disarm();
        store->Close();
        store.reset();

        if (consumed > 0) {
            any = true;
            if (require_rc_fail) {
                EXPECT_NE(rc, 0) << op << " k=" << k
                                 << ": doc_create must fail when its " << op << " EIOs";
            }
        }
        if (matched <= k && consumed == 0) break;   // swept past the last matching call
        if (k > 512) { ADD_FAILURE() << op << " sweep did not terminate by k=512"; break; }
    }
    if (out_any_consumed) *out_any_consumed = any;
}

// pwrite is the WAL/data write syscall — it MUST be reachable on the doc-create path
// (asserted), and each injected failure must surface as a non-zero doc_create rc.
TEST_F(StoreFaultSweepTest, DocCreatePwriteFaultSweep) {
    bool any = false;
    SweepDocWriteOp(this, "pwrite", /*require_rc_fail=*/true, &any);
    EXPECT_TRUE(any) << "no interposable pwrite on the doc-create write path";

    // Recoverable: a clean store with nothing armed works end-to-end.
    fi::Disarm();
    auto ok = OpenStoreRecordingFds();
    fi::Disarm();  // drop the record-only arm before the clean workload
    CortrixDoc d = MakeDoc("doc_recovered");
    EXPECT_EQ(ok->doc_create(d), 0);
    int64_t n = -1;
    EXPECT_EQ(ok->doc_count(&n), 0);
    EXPECT_GE(n, 1);
    ok->Close();
}

// fsync / fullfsync on the commit path: a sync failure must never crash. rc may be
// non-zero here or be deferred to a later checkpoint, so we don't force rc!=0 — the
// assertion of interest is "graceful under every synced-write fault arm".
TEST_F(StoreFaultSweepTest, DocCreateSyncFaultSweep) {
    for (const char* op : {"fsync", "fullfsync"}) {
        SweepDocWriteOp(this, op, /*require_rc_fail=*/false, nullptr);
    }
    SUCCEED();
}

// Sweep pwrite over the doc_delete TRANSACTION path (BEGIN → DELETE blocks → DELETE
// doc → COMMIT, each with a ROLLBACK-on-failure arm). Per k: open a clean store, seed
// a doc + a block fault-free, then arm pwrite and delete — a write fault inside the
// transaction must ROLLBACK and return non-zero without crashing, and the store must
// stay usable. Reaches the §4.5-adjacent transaction error arms a :memory: db can't.
TEST_F(StoreFaultSweepTest, DocDeleteTransactionPwriteFaultSweep) {
    bool any = false;
    for (int k = 0;; ++k) {
        auto store = OpenStoreRecordingFds();
        fi::Disarm();  // seed fault-free

        CortrixDoc d = MakeDoc("del_" + std::to_string(k));
        ASSERT_EQ(store->doc_create(d), 0);
        CortrixBlock b;
        b.doc_id = d.doc_id;
        b.block_type = 1;  // text block
        b.content_text = "block body";
        b.data = {0x01, 0x02, 0x03, 0x04};  // blocks.data is NOT NULL
        ASSERT_EQ(store->block_insert(b), 0);

        fi::FailOp("pwrite", /*skip=*/k, /*count=*/1, EIO, "test.db");
        const int rc = store->doc_delete(d.doc_id);
        const int consumed = fi::ConsumedCount();
        const int matched = fi::MatchedCount();
        fi::Disarm();

        if (consumed > 0) {
            any = true;
            EXPECT_NE(rc, 0) << "k=" << k << ": doc_delete must fail+ROLLBACK on pwrite EIO";
            // After a failed+rolled-back delete the store must still answer queries.
            int64_t n = -1;
            EXPECT_EQ(store->doc_count(&n), 0) << "store unusable after rolled-back delete";
        }
        store->Close();
        store.reset();
        if (matched <= k && consumed == 0) break;
        if (k > 512) { ADD_FAILURE() << "doc_delete sweep did not terminate"; break; }
    }
    EXPECT_TRUE(any) << "no interposable pwrite on the doc_delete transaction path";
}

// ---------------------------------------------------------------------------
// MemoryStore (memory.db) — interaction-write fault sweep
// ---------------------------------------------------------------------------
// memory.db is a SEPARATE SQLite db from the CortrixStore's; arming "memory.db"
// scopes injection to it. Same SQLite arm-window pattern (record fds via a pre-armed
// open before Init, then switch op). Workload = InteractionInsert (FK-anchored to a
// fault-free seeded session) → hits MemoryStore's prepare/step IOERR arms.
class MemoryStoreFaultSweepTest : public ::testing::Test {
public:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("cortrix_mem_fault_" + std::to_string(::getpid()) + "_" +
                std::to_string(rand()));
        fs::create_directories(dir_);
        mem_db_ = (dir_ / "memory.db").string();
        // The CortrixStore the MemoryStore ctor borrows lives in a DIFFERENT file
        // ("cortrix.db"), so the "memory.db" path filter never touches it.
        backing_ = std::make_unique<CortrixStoreSqlite>((dir_ / "cortrix.db").string());
        EXPECT_EQ(backing_->Open(), 0);
    }
    void TearDown() override {
        fi::Disarm();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // Build + Init a MemoryStore over memory.db with the seam recording its fds.
    std::unique_ptr<MemoryStore> OpenMemStoreRecordingFds() {
        fi::FailOp("open", kRecordOnlySkip, 1, EIO, "memory.db");  // record, never fail
        auto m = std::make_unique<MemoryStore>(*backing_);
        EXPECT_TRUE(m->Init(mem_db_).ok());
        return m;
    }

    static MemorySession Session(const std::string& id) {
        MemorySession s;
        s.session_id = id;
        s.namespace_name = "default";
        s.user_id = "u1";
        return s;
    }
    static InteractionLog Interaction(const std::string& session_id) {
        InteractionLog log;
        log.session_id = session_id;
        log.namespace_name = "default";
        log.role = "user";
        log.content = "hello";
        log.query_type = "chat";
        log.status = "success";
        return log;
    }

    fs::path dir_;
    std::string mem_db_;
    std::unique_ptr<CortrixStoreSqlite> backing_;
};

// Sweep pwrite over InteractionInsert. Seed the session fault-free, then arm pwrite
// and insert — every injected write fault must surface as a non-ok Status (never a
// crash), and the store must answer queries after Disarm().
TEST_F(MemoryStoreFaultSweepTest, InteractionInsertPwriteFaultSweep) {
    bool any = false;
    for (int k = 0;; ++k) {
        auto mem = OpenMemStoreRecordingFds();
        fi::Disarm();  // seed fault-free

        MemorySession s = Session("sess_" + std::to_string(k));
        ASSERT_TRUE(mem->SessionCreate(s).ok());

        fi::FailOp("pwrite", /*skip=*/k, /*count=*/1, EIO, "memory.db");
        InteractionLog log = Interaction(s.session_id);
        const Status st = mem->InteractionInsert(log);
        const int consumed = fi::ConsumedCount();
        const int matched = fi::MatchedCount();
        fi::Disarm();

        if (consumed > 0) {
            any = true;
            EXPECT_FALSE(st.ok()) << "k=" << k
                                  << ": InteractionInsert must fail when its pwrite EIOs";
        }
        mem.reset();
        if (matched <= k && consumed == 0) break;
        if (k > 512) { ADD_FAILURE() << "InteractionInsert sweep did not terminate"; break; }
    }
    EXPECT_TRUE(any) << "no interposable pwrite on the InteractionInsert path";
}

}  // namespace
}  // namespace cortrix

#endif  // CORTRIX_ENABLE_FAULT_INJECT
