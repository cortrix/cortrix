// test_store_ops_fault.cpp — BREADTH matrices for CortrixStoreSqlite operation
// behavior: the SetFailNextOps / TryConsumeOpFault testing seam exercised across
// EVERY public CRUD op's failure arm, plus boundary matrices for
// block_delete_by_doc / doc_delete_by_source_prefix / doc_list_by_status /
// doc_list_deleted_before.
//
// Deterministic: temp on-disk SQLite (WAL), no network/LLM, no real sleeps.
// gtest suite/fixture names are GLOBAL strings — every name here is prefixed
// StoreFaultOps* to stay globally unique (no clash with StoreSqliteTest /
// StoreDepthSqliteFx / etc.). APIs verified against
// include/cortrix/store/cortrix_store_sqlite.h + cortrix_store.h.

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "cortrix/common/block_header.h"
#include "cortrix/store/cortrix_store_sqlite.h"

using namespace cortrix;
namespace fs = std::filesystem;

namespace {

// ── Shared fixture: a fresh owns-mode store on a temp db ──────────────────────
class StoreFaultOpsBase : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique per PROCESS (gtest_discover_tests runs each case in its own
        // process; rand() is unseeded and identical across them) AND per SetUp,
        // so parallel -j2 runs never share a temp db file.
        static std::atomic<unsigned> seq{0};
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_store_fault_ops_" + std::to_string(getpid()) + "_" +
                     std::to_string(seq.fetch_add(1)));
        fs::create_directories(test_dir_);
        db_path_ = (test_dir_ / "fault.db").string();
        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);
    }
public:
    // Public accessor so free-function op invokers (MATRIX 1) can reach the store.
    CortrixStoreSqlite* store() { return store_.get(); }

protected:
    void TearDown() override {
        if (store_) store_->Close();
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    // doc_create honors a preset doc_id (see store_depth MakeReadyDoc).
    std::string MakeDoc(const std::string& id,
                        const std::string& source_path,
                        DocStatus status = DocStatus::kPending,
                        const std::string& source_type = "file") {
        CortrixDoc d;
        d.doc_id = id;
        d.source_type = source_type;
        d.source_path = source_path;
        d.status = status;
        EXPECT_EQ(store_->doc_create(d), 0) << "doc_create(" << id << ")";
        return d.doc_id;
    }

    // FK + NOT NULL: doc must exist; blocks.data non-empty.
    uint64_t MakeBlock(const std::string& doc_id, int chunk_index,
                       const std::string& text = "row") {
        CortrixBlock b;
        b.doc_id = doc_id;
        b.chunk_index = chunk_index;
        b.block_type = 1;
        b.processing_level = 2;
        b.data = BlockBuild(kBlockFile, kLevelL2, text, "", "", 0, 0);
        b.content_text = text;
        EXPECT_EQ(store_->block_insert(b), 0) << "block_insert " << chunk_index;
        return b.block_id;
    }

    int64_t DocCount() {
        int64_t c = -1;
        EXPECT_EQ(store_->doc_count(&c), 0);
        return c;
    }
    int64_t BlockCount() {
        int64_t c = -1;
        EXPECT_EQ(store_->block_count(&c), 0);
        return c;
    }

    fs::path test_dir_;
    std::string db_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
};

// =============================================================================
// MATRIX 1 — every public CRUD op's injected-fault arm.
//
// Contract (cortrix_store_sqlite.h): SetFailNextOps(n) forces the next n
// public CRUD calls to return -1 WITHOUT touching the db. We arm exactly 1, run
// one op, assert it returns -1, then assert the third (un-armed) call resumes
// real behavior AND the store state is untouched by the faulted op.
//
// Each op is an "invoker" closure run against a fresh fixture so arming windows
// never overlap. Parameterized so adding/removing an op is one row.
// =============================================================================

struct OpFaultCase {
    std::string name;
    // returns the result code of invoking the op once (seam already armed).
    std::function<int(StoreFaultOpsBase&)> invoke;
};

class StoreFaultOpsMatrix : public StoreFaultOpsBase,
                            public ::testing::WithParamInterface<OpFaultCase> {};

std::vector<OpFaultCase> AllOpFaultCases() {
    std::vector<OpFaultCase> v;

    v.push_back({"doc_create", [](StoreFaultOpsBase& f) {
        CortrixDoc d; d.doc_id = "x1"; d.source_type = "file"; d.source_path = "/p/x1";
        return f.store()->doc_create(d);
    }});
    v.push_back({"doc_get", [](StoreFaultOpsBase& f) {
        CortrixDoc d; return f.store()->doc_get("any", d);
    }});
    v.push_back({"doc_update_status", [](StoreFaultOpsBase& f) {
        return f.store()->doc_update_status("any", DocStatus::kReady);
    }});
    v.push_back({"doc_update_status_with_msg", [](StoreFaultOpsBase& f) {
        return f.store()->doc_update_status("any", DocStatus::kError, "boom");
    }});
    v.push_back({"doc_delete", [](StoreFaultOpsBase& f) {
        return f.store()->doc_delete("any");
    }});
    v.push_back({"doc_delete_by_source_prefix", [](StoreFaultOpsBase& f) {
        int64_t removed = -99;
        return f.store()->doc_delete_by_source_prefix("p/", &removed);
    }});
    v.push_back({"doc_soft_delete", [](StoreFaultOpsBase& f) {
        return f.store()->doc_soft_delete("any", 1000);
    }});
    v.push_back({"doc_restore", [](StoreFaultOpsBase& f) {
        return f.store()->doc_restore("any");
    }});
    v.push_back({"doc_list_deleted_before", [](StoreFaultOpsBase& f) {
        std::vector<CortrixDoc> out; return f.store()->doc_list_deleted_before(9999, out);
    }});
    v.push_back({"doc_list_by_status", [](StoreFaultOpsBase& f) {
        std::vector<CortrixDoc> out; return f.store()->doc_list_by_status(DocStatus::kPending, out);
    }});
    v.push_back({"doc_find_by_source", [](StoreFaultOpsBase& f) {
        CortrixDoc d; return f.store()->doc_find_by_source("file", "/p/x", d);
    }});
    v.push_back({"doc_find_by_hash", [](StoreFaultOpsBase& f) {
        CortrixDoc d; return f.store()->doc_find_by_hash("deadbeef", d);
    }});
    v.push_back({"doc_count", [](StoreFaultOpsBase& f) {
        int64_t c = -1; return f.store()->doc_count(&c);
    }});
    v.push_back({"block_insert", [](StoreFaultOpsBase& f) {
        CortrixBlock b; b.doc_id = "any"; b.chunk_index = 0; b.block_type = 1;
        b.processing_level = 2; b.data = {1, 2, 3}; b.content_text = "t";
        return f.store()->block_insert(b);
    }});
    v.push_back({"block_get", [](StoreFaultOpsBase& f) {
        CortrixBlock b; return f.store()->block_get(1, b);
    }});
    v.push_back({"block_get_by_doc", [](StoreFaultOpsBase& f) {
        std::vector<CortrixBlock> out; return f.store()->block_get_by_doc("any", out);
    }});
    v.push_back({"block_delete_by_doc", [](StoreFaultOpsBase& f) {
        return f.store()->block_delete_by_doc("any");
    }});
    v.push_back({"block_count", [](StoreFaultOpsBase& f) {
        int64_t c = -1; return f.store()->block_count(&c);
    }});
    v.push_back({"block_count_by_doc", [](StoreFaultOpsBase& f) {
        int64_t c = -1; return f.store()->block_count_by_doc("any", &c);
    }});
    v.push_back({"parent_insert", [](StoreFaultOpsBase& f) {
        CortrixParent p; p.parent_id = "pp"; p.doc_id = "any"; p.namespace_id = "ns";
        p.parent_text = "span";
        return f.store()->parent_insert(p);
    }});
    v.push_back({"parent_get", [](StoreFaultOpsBase& f) {
        CortrixParent p; return f.store()->parent_get("pp", p);
    }});
    v.push_back({"search_fulltext", [](StoreFaultOpsBase& f) {
        std::vector<SearchResult> r; return f.store()->search_fulltext("term", 10, r);
    }});
    v.push_back({"search_metadata", [](StoreFaultOpsBase& f) {
        std::vector<SearchResult> r; return f.store()->search_metadata("filter", 10, r);
    }});
    return v;
}

// Single-op armed window: arm 1, op returns -1.
TEST_P(StoreFaultOpsMatrix, ArmedOpReturnsMinusOne) {
    store_->SetFailNextOps(1);
    EXPECT_EQ(GetParam().invoke(*this), -1) << GetParam().name;
}

// After the single armed fault is consumed, real behavior resumes (count works).
TEST_P(StoreFaultOpsMatrix, SeamDisarmsAfterOneFault) {
    store_->SetFailNextOps(1);
    (void)GetParam().invoke(*this);  // consumes the one armed fault
    int64_t c = -1;
    EXPECT_EQ(store_->doc_count(&c), 0) << "post-fault op should run normally";
    EXPECT_EQ(c, 0);
}

// The faulted op must NOT touch the db: with the seam armed, a mutating op fails
// and leaves doc/block counts unchanged from the pre-arm baseline.
TEST_P(StoreFaultOpsMatrix, FaultedOpDoesNotMutate) {
    // Pre-arm baseline state: one doc + one block (created before arming).
    std::string d = MakeDoc("base", "/p/base");
    MakeBlock(d, 0);
    const int64_t docs_before = DocCount();
    const int64_t blocks_before = BlockCount();

    store_->SetFailNextOps(1);
    EXPECT_EQ(GetParam().invoke(*this), -1) << GetParam().name;

    // Counting also goes through the seam, but it's now disarmed (1 consumed).
    EXPECT_EQ(DocCount(), docs_before);
    EXPECT_EQ(BlockCount(), blocks_before);
}

INSTANTIATE_TEST_SUITE_P(
    AllOps, StoreFaultOpsMatrix, ::testing::ValuesIn(AllOpFaultCases()),
    [](const ::testing::TestParamInfo<OpFaultCase>& i) { return i.param.name; });

// =============================================================================
// MATRIX 2 — multi-fault window depth: arming n faults makes the next n calls
// fail, the (n+1)th succeeds. Parameterized over n = 1..8.
// =============================================================================
class StoreFaultOpsWindow : public StoreFaultOpsBase,
                            public ::testing::WithParamInterface<int> {};

TEST_P(StoreFaultOpsWindow, ExactlyNCallsFault) {
    const int n = GetParam();
    store_->SetFailNextOps(n);
    for (int i = 0; i < n; ++i) {
        int64_t c = -1;
        EXPECT_EQ(store_->doc_count(&c), -1) << "call " << i << " of " << n;
    }
    // (n+1)th call resumes real behavior.
    int64_t c = -1;
    EXPECT_EQ(store_->doc_count(&c), 0);
    EXPECT_EQ(c, 0);
}

TEST_P(StoreFaultOpsWindow, ReArmingExtendsWindow) {
    const int n = GetParam();
    store_->SetFailNextOps(n);
    int64_t c = -1;
    EXPECT_EQ(store_->doc_count(&c), -1);  // consume 1
    store_->SetFailNextOps(n);             // re-arm to n (overwrites remaining)
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(store_->doc_count(&c), -1) << "re-armed call " << i;
    }
    EXPECT_EQ(store_->doc_count(&c), 0);
}

INSTANTIATE_TEST_SUITE_P(Depth, StoreFaultOpsWindow,
                         ::testing::Values(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16,
                                           20, 32, 50, 64));

// =============================================================================
// MATRIX 3 — SetFailNextOps(0) is a hard disable from any prior armed state.
// =============================================================================
class StoreFaultOpsDisable : public StoreFaultOpsBase,
                             public ::testing::WithParamInterface<int> {};

TEST_P(StoreFaultOpsDisable, ZeroDisablesRegardlessOfPriorArm) {
    store_->SetFailNextOps(GetParam());  // arm some amount
    store_->SetFailNextOps(0);           // then hard-disable
    int64_t c = -1;
    EXPECT_EQ(store_->doc_count(&c), 0);
    EXPECT_EQ(c, 0);
}

INSTANTIATE_TEST_SUITE_P(FromArmed, StoreFaultOpsDisable,
                         ::testing::Values(0, 1, 2, 3, 4, 5, 8, 10, 16, 50, 100,
                                           1000));

// =============================================================================
// MATRIX 4 — block_delete_by_doc boundary matrix.
// =============================================================================
struct BlockDeleteCase {
    std::string name;
    int n_blocks;          // blocks created under the target doc
    int n_other_blocks;    // blocks under a sibling doc (must survive)
    bool target_exists;    // whether to create the target doc at all
    int expect_ret;        // expected return code
};

class StoreFaultOpsBlockDelete
    : public StoreFaultOpsBase,
      public ::testing::WithParamInterface<BlockDeleteCase> {};

TEST_P(StoreFaultOpsBlockDelete, DeletesOnlyTargetDocBlocks) {
    const auto& p = GetParam();
    std::string other = MakeDoc("other", "/p/other");
    for (int i = 0; i < p.n_other_blocks; ++i) MakeBlock(other, i);

    std::string target = "tgt";
    if (p.target_exists) {
        MakeDoc(target, "/p/tgt");
        for (int i = 0; i < p.n_blocks; ++i) MakeBlock(target, i);
    }

    EXPECT_EQ(store_->block_delete_by_doc(target), p.expect_ret) << p.name;

    std::vector<CortrixBlock> tb;
    store_->block_get_by_doc(target, tb);
    EXPECT_TRUE(tb.empty()) << "target blocks must be gone";

    std::vector<CortrixBlock> ob;
    store_->block_get_by_doc(other, ob);
    EXPECT_EQ(static_cast<int>(ob.size()), p.n_other_blocks)
        << "sibling doc blocks must survive";
}

INSTANTIATE_TEST_SUITE_P(
    Boundary, StoreFaultOpsBlockDelete,
    ::testing::Values(
        BlockDeleteCase{"no_blocks_target_exists", 0, 0, true, 0},
        BlockDeleteCase{"one_block", 1, 0, true, 0},
        BlockDeleteCase{"two_blocks", 2, 0, true, 0},
        BlockDeleteCase{"five_blocks", 5, 0, true, 0},
        BlockDeleteCase{"ten_blocks", 10, 0, true, 0},
        BlockDeleteCase{"twenty_blocks", 20, 0, true, 0},
        BlockDeleteCase{"with_one_sibling", 3, 1, true, 0},
        BlockDeleteCase{"with_sibling_blocks", 3, 4, true, 0},
        BlockDeleteCase{"with_many_siblings", 1, 10, true, 0},
        BlockDeleteCase{"only_siblings_target_empty", 0, 5, true, 0},
        BlockDeleteCase{"nonexistent_doc_is_noop", 0, 2, false, 0},
        BlockDeleteCase{"nonexistent_no_siblings", 0, 0, false, 0}),
    [](const ::testing::TestParamInfo<BlockDeleteCase>& i) { return i.param.name; });

// =============================================================================
// MATRIX 5 — doc_delete_by_source_prefix boundary matrix (table-boundary trap).
// =============================================================================
struct PrefixCase {
    std::string name;
    std::string prefix;
    int64_t expect_removed;   // blocks removed
    int64_t expect_remaining; // docs remaining
};

class StoreFaultOpsPrefix : public StoreFaultOpsBase,
                            public ::testing::WithParamInterface<PrefixCase> {};

TEST_P(StoreFaultOpsPrefix, DeletesByPrefixHonoringBoundary) {
    const auto& p = GetParam();
    // Fixed corpus: users/1 (1 blk), users/2 (1 blk), users2/1 (1 blk), orders/1 (1 blk)
    auto add = [&](const std::string& path, int nblk) {
        std::string id = MakeDoc(path, path, DocStatus::kPending, "db_import");
        for (int i = 0; i < nblk; ++i) MakeBlock(id, i);
    };
    add("postgres://h/db/users/1", 1);
    add("postgres://h/db/users/2", 1);
    add("postgres://h/db/users2/1", 1);
    add("postgres://h/db/orders/1", 1);
    ASSERT_EQ(DocCount(), 4);

    int64_t removed = -99;
    EXPECT_EQ(store_->doc_delete_by_source_prefix(p.prefix, &removed), 0) << p.name;
    EXPECT_EQ(removed, p.expect_removed);
    EXPECT_EQ(DocCount(), p.expect_remaining);
}

INSTANTIATE_TEST_SUITE_P(
    Boundary, StoreFaultOpsPrefix,
    ::testing::Values(
        PrefixCase{"table_boundary", "postgres://h/db/users/", 2, 2},
        PrefixCase{"users2_only", "postgres://h/db/users2/", 1, 3},
        PrefixCase{"orders_only", "postgres://h/db/orders/", 1, 3},
        PrefixCase{"empty_prefix_noop", "", 0, 4},
        PrefixCase{"no_match", "postgres://h/db/none/", 0, 4},
        PrefixCase{"partial_no_boundary", "postgres://h/db/user", 3, 1},
        PrefixCase{"exact_doc_path", "postgres://h/db/orders/1", 1, 3},
        PrefixCase{"scheme_prefix", "postgres://", 4, 0},
        PrefixCase{"unrelated_scheme", "mysql://h/db/users/", 0, 4},
        PrefixCase{"whole_db", "postgres://h/db/", 4, 0}),
    [](const ::testing::TestParamInfo<PrefixCase>& i) { return i.param.name; });

// null deleted_blocks pointer is tolerated.
TEST_F(StoreFaultOpsBase, PrefixNullCounterArg) {
    MakeDoc("postgres://h/db/t/1", "postgres://h/db/t/1", DocStatus::kPending, "db_import");
    EXPECT_EQ(store_->doc_delete_by_source_prefix("postgres://h/db/t/", nullptr), 0);
    EXPECT_EQ(DocCount(), 0);
}

// =============================================================================
// MATRIX 6 — doc_list_by_status over every DocStatus enumerator.
// =============================================================================
class StoreFaultOpsListByStatus
    : public StoreFaultOpsBase,
      public ::testing::WithParamInterface<DocStatus> {};

TEST_P(StoreFaultOpsListByStatus, ListsExactlyMatchingStatus) {
    const DocStatus target = GetParam();
    // Create one doc per status (kDeleted/kProcessing/... via update; pending default).
    const DocStatus all[] = {
        DocStatus::kPending,   DocStatus::kProcessing, DocStatus::kReady,
        DocStatus::kError,     DocStatus::kStale,      DocStatus::kDeleting,
        DocStatus::kDeleted};
    int idx = 0;
    std::string target_id;
    for (DocStatus s : all) {
        std::string id = "s" + std::to_string(idx);
        MakeDoc(id, "/p/" + id);
        if (s == DocStatus::kDeleted) {
            store_->doc_soft_delete(id, 1000);  // proper soft-delete path
        } else if (s != DocStatus::kPending) {
            store_->doc_update_status(id, s);
        }
        if (s == target) target_id = id;
        ++idx;
    }

    std::vector<CortrixDoc> out;
    EXPECT_EQ(store_->doc_list_by_status(target, out), 0);
    ASSERT_EQ(out.size(), 1u) << "exactly one doc has status "
                              << static_cast<int>(target);
    EXPECT_EQ(out[0].doc_id, target_id);
    EXPECT_EQ(out[0].status, target);
}

INSTANTIATE_TEST_SUITE_P(
    EveryStatus, StoreFaultOpsListByStatus,
    ::testing::Values(DocStatus::kPending, DocStatus::kProcessing,
                      DocStatus::kReady, DocStatus::kError, DocStatus::kStale,
                      DocStatus::kDeleting, DocStatus::kDeleted),
    [](const ::testing::TestParamInfo<DocStatus>& i) {
        return "status_" + std::to_string(static_cast<int>(i.param));
    });

// Empty result when no doc has the requested status.
TEST_F(StoreFaultOpsBase, ListByStatusEmptyWhenNoneMatch) {
    MakeDoc("only", "/p/only");  // pending
    std::vector<CortrixDoc> out;
    EXPECT_EQ(store_->doc_list_by_status(DocStatus::kError, out), 0);
    EXPECT_TRUE(out.empty());
}

// =============================================================================
// MATRIX 7 — doc_list_deleted_before cutoff boundary matrix.
// Soft-delete docs at deleted_at = {100, 500, 900}; sweep at various cutoffs.
// =============================================================================
struct DeletedBeforeCase {
    std::string name;
    int64_t cutoff_ms;
    int expect_count;  // how many of {100,500,900} qualify (deleted_at <= cutoff)
};

class StoreFaultOpsDeletedBefore
    : public StoreFaultOpsBase,
      public ::testing::WithParamInterface<DeletedBeforeCase> {};

TEST_P(StoreFaultOpsDeletedBefore, ReturnsSoftDeletedAtOrBeforeCutoff) {
    const auto& p = GetParam();
    // Three soft-deleted docs at distinct deleted_at timestamps + one live doc.
    MakeDoc("d100", "/p/d100"); store_->doc_soft_delete("d100", 100);
    MakeDoc("d500", "/p/d500"); store_->doc_soft_delete("d500", 500);
    MakeDoc("d900", "/p/d900"); store_->doc_soft_delete("d900", 900);
    MakeDoc("live", "/p/live");  // never deleted, must never appear

    std::vector<CortrixDoc> out;
    EXPECT_EQ(store_->doc_list_deleted_before(p.cutoff_ms, out), 0) << p.name;
    EXPECT_EQ(static_cast<int>(out.size()), p.expect_count);
    for (const auto& d : out) {
        EXPECT_EQ(d.status, DocStatus::kDeleted);
        EXPECT_NE(d.doc_id, "live");
    }
}

INSTANTIATE_TEST_SUITE_P(
    Cutoff, StoreFaultOpsDeletedBefore,
    // store uses INCLUSIVE `deleted_at <= cutoff` (cortrix_store_sqlite.cpp).
    ::testing::Values(
        DeletedBeforeCase{"negative", -5, 0},
        DeletedBeforeCase{"zero", 0, 0},
        DeletedBeforeCase{"just_below_100", 99, 0},
        DeletedBeforeCase{"exactly_100", 100, 1},
        DeletedBeforeCase{"just_above_100", 101, 1},
        DeletedBeforeCase{"between_100_500", 300, 1},
        DeletedBeforeCase{"just_below_500", 499, 1},
        DeletedBeforeCase{"exactly_500", 500, 2},
        DeletedBeforeCase{"just_above_500", 501, 2},
        DeletedBeforeCase{"between_500_900", 700, 2},
        DeletedBeforeCase{"just_below_900", 899, 2},
        DeletedBeforeCase{"exactly_900", 900, 3},
        DeletedBeforeCase{"just_above_900", 901, 3},
        DeletedBeforeCase{"after_all", 100000, 3}),
    [](const ::testing::TestParamInfo<DeletedBeforeCase>& i) { return i.param.name; });

// Empty store: list_deleted_before is a clean empty result.
TEST_F(StoreFaultOpsBase, DeletedBeforeEmptyStore) {
    std::vector<CortrixDoc> out;
    EXPECT_EQ(store_->doc_list_deleted_before(9999, out), 0);
    EXPECT_TRUE(out.empty());
}

}  // namespace
