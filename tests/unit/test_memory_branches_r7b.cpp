// R7-1d — branch-coverage supplements for the memory data layer.
// Targets reachable error/edge branches the existing memory suites leave
// uncovered (driving memory_block_adapter.cpp 56.8% branch):
//
//   B1  InsertMemoryBlock WITH an embedder + vec index — the embed + AddPoints
//       success path (existing test_memory_block_adapter passes nullptr/nullptr,
//       so only the no-vector degrade path is covered).
//   B2  InsertMemoryBlock with a vec index whose AddPoints FAILS — the warn arm.
//   B3  InsertMemoryBlock with a non-object metadata_json — the `is_object()`
//       false → empty-object fallback arm (Insert) + the empty user_id arm.
//   B4  ToRecord over a malformed metadata_json (discarded / non-object) — the
//       else branch that resets metadata to an empty object.
//   B5  GetMemoryBlock on a missing id — the NotFound arm.
//   B6  QueryUserFacts limit<=0 → default 20, FindCandidates top_k<=0 → default 5.
//
// All reachable with a real :memory: CortrixStoreSqlite + a stub OnnxEmbedder
// (empty model path → deterministic stub vectors) + a tiny fake IIndex. No
// threads, no source changes.
#include "cortrix/memory/memory_block_adapter.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

#include "cortrix/catalog/bloom_filter.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/config/config.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/iindex.h"

namespace cortrix::memory {
namespace {

// Minimal IIndex fake: records AddPoints calls; AddPoints can be made to fail to
// exercise the InsertMemoryBlock warn arm. Every other op is a trivial success
// stub (memory insert only calls AddPoints).
class FakeIndex : public store::IIndex {
public:
    Status AddPoint(const float*, uint64_t,
                    const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>& pts,
                     const observability::TraceContext*) override {
        add_points_calls += 1;
        last_batch_size = pts.size();
        if (!pts.empty()) last_block_id = pts.front().second;
        return fail_add_ ? Status::Internal("CX_ERR_INDEX: forced AddPoints failure")
                         : Status::Ok();
    }
    Status MarkDelete(uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    std::vector<std::pair<uint64_t, float>> Search(
        const float*, int, int, const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t) override { return false; }
    Status Snapshot() override { return Status::Ok(); }
    Status Recover() override { return Status::Ok(); }
    Status Shutdown() override { return Status::Ok(); }
    store::IndexStats GetStats() override { return {}; }
    std::size_t GetMemoryFootprintBytes() const override { return 0; }

    void set_fail_add(bool v) { fail_add_ = v; }
    int add_points_calls = 0;
    std::size_t last_batch_size = 0;
    uint64_t last_block_id = 0;

private:
    bool fail_add_ = false;
};

class MemoryBranchesTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<CortrixStoreSqlite>(":memory:");
        ASSERT_EQ(store_->Open(), 0);
        // Empty model path → OnnxEmbedder runs in stub mode (deterministic
        // non-empty vectors), so the embed + index path is exercised without a
        // real model on disk.
        embedder_ = std::make_unique<OnnxEmbedder>("", /*dim=*/64);
        ASSERT_TRUE(embedder_->Init().ok());
    }

    static MemoryBlockRecord Rec(const std::string& id, const std::string& user,
                                 const std::string& content, const nlohmann::json& meta) {
        MemoryBlockRecord r;
        r.block_id = id;
        r.user_id = user;
        r.content = content;
        r.metadata_json = meta;
        return r;
    }

    std::unique_ptr<CortrixStoreSqlite> store_;
    std::unique_ptr<OnnxEmbedder> embedder_;
};

// B1 — Insert with embedder + index: the content is embedded and the vector is
// added to the index (have_vec && vec_index_ success arm).
TEST_F(MemoryBranchesTest, InsertWithEmbedderAddsVectorToIndex) {
    FakeIndex index;
    MemoryBlockAdapter adapter(*store_, embedder_.get(), &index);

    auto meta = nlohmann::json{{"memory_type", "fact"}, {"status", "active"},
                               {"user_id", "alice"}, {"created_at", "2026-06-12T10:00:00Z"}};
    auto ins = adapter.InsertMemoryBlock(Rec("01MEMVEC", "alice", "user likes tea", meta));
    ASSERT_TRUE(ins.ok()) << ins.status().message();
    EXPECT_EQ(ins.value(), "01MEMVEC");

    // The embedder produced a vector → AddPoints was called once with one point.
    EXPECT_EQ(index.add_points_calls, 1);
    EXPECT_EQ(index.last_batch_size, 1u);
    EXPECT_NE(index.last_block_id, 0u);

    // Row is still stored + readable.
    auto got = adapter.GetMemoryBlock("01MEMVEC");
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().content, "user likes tea");
}

// B2 — Insert with an index whose AddPoints fails: the failure is logged +
// swallowed (the row insert still succeeds; warn arm covered).
TEST_F(MemoryBranchesTest, InsertVectorIndexFailureIsNonFatal) {
    FakeIndex index;
    index.set_fail_add(true);
    MemoryBlockAdapter adapter(*store_, embedder_.get(), &index);

    auto meta = nlohmann::json{{"memory_type", "fact"}, {"status", "active"},
                               {"user_id", "bob"}, {"created_at", "2026-06-12T10:00:00Z"}};
    auto ins = adapter.InsertMemoryBlock(Rec("01MEMVECF", "bob", "user likes coffee", meta));
    ASSERT_TRUE(ins.ok()) << "a vector-index failure must not fail the row insert";
    EXPECT_EQ(index.add_points_calls, 1);  // AddPoints attempted (and failed)

    // Row persisted despite the index failure.
    auto got = adapter.GetMemoryBlock("01MEMVECF");
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().content, "user likes coffee");
}

// B3 — Insert with a non-object metadata_json + empty user_id: the is_object()
// false fallback runs, and the empty-user_id arm (no user_id injected) is taken.
TEST_F(MemoryBranchesTest, InsertNonObjectMetadataAndEmptyUserId) {
    MemoryBlockAdapter adapter(*store_, nullptr, nullptr);

    // metadata_json is an array (not an object) → adapter falls back to a fresh
    // object and injects block_id; user_id is empty → not injected.
    MemoryBlockRecord r;
    r.block_id = "01MEMNOOBJ";
    r.user_id = "";  // empty → the `!block.user_id.empty()` arm is false
    r.content = "no-meta content";
    r.metadata_json = nlohmann::json::array({1, 2, 3});

    auto ins = adapter.InsertMemoryBlock(r);
    ASSERT_TRUE(ins.ok()) << ins.status().message();

    auto got = adapter.GetMemoryBlock("01MEMNOOBJ");
    ASSERT_TRUE(got.ok());
    // block_id echoed back from the rebuilt object; no user_id present.
    EXPECT_EQ(got.value().block_id, "01MEMNOOBJ");
    EXPECT_TRUE(got.value().user_id.empty());
}

// B4 — GetMemoryBlock over a row whose metadata_json is malformed: ToRecord's
// parse is discarded → it resets metadata to an empty object (else arm) and the
// block_id falls back to the lookup id.
TEST_F(MemoryBranchesTest, GetWithMalformedMetadataResetsToEmptyObject) {
    // Insert a normal row first, then corrupt its metadata_json directly so the
    // stored JSON no longer parses.
    MemoryBlockAdapter adapter(*store_, nullptr, nullptr);
    auto meta = nlohmann::json{{"memory_type", "fact"}, {"user_id", "carol"},
                               {"status", "active"}, {"created_at", "2026-06-12T10:00:00Z"}};
    ASSERT_TRUE(adapter.InsertMemoryBlock(Rec("01MEMBAD", "carol", "c", meta)).ok());

    // Overwrite metadata_json with non-JSON garbage directly in the DB. block_get
    // does a fresh SELECT (no row cache), so the corrupted value reaches ToRecord,
    // whose nlohmann parse is discarded → the empty-object reset arm (block_adapter
    // .cpp:60-62). NOTE: memory blocks are block_type = kBlockMemory = 7 (NOT 4) —
    // the earlier `block_type=4` matched zero rows, leaving valid JSON in place.
    sqlite3* db = store_->db_handle();
    ASSERT_NE(db, nullptr);
    int updated = -1;
    ASSERT_EQ(sqlite3_exec(db,
        "UPDATE blocks SET metadata_json='{not valid json' WHERE block_type=7",
        nullptr, nullptr, nullptr), SQLITE_OK);
    updated = sqlite3_changes(db);
    ASSERT_EQ(updated, 1) << "exactly the one memory block must be corrupted";

    auto got = adapter.GetMemoryBlock("01MEMBAD");
    ASSERT_TRUE(got.ok());
    // Malformed JSON → metadata reset to empty object, block_id falls back to arg.
    EXPECT_TRUE(got.value().metadata_json.is_object());
    EXPECT_TRUE(got.value().metadata_json.empty());
    EXPECT_EQ(got.value().block_id, "01MEMBAD");
}

// B5 — GetMemoryBlock on an id that was never inserted → NotFound.
TEST_F(MemoryBranchesTest, GetMissingBlockReturnsNotFound) {
    MemoryBlockAdapter adapter(*store_, nullptr, nullptr);
    auto got = adapter.GetMemoryBlock("01NOSUCHBLOCK");
    EXPECT_FALSE(got.ok());
    EXPECT_EQ(got.status().code(), StatusCode::kNotFound);
    EXPECT_NE(got.status().message().find("CX_ERR_MEMEXTRACT_STORE"), std::string::npos);
}

// B6 — limit<=0 / top_k<=0 select the default caps (QueryUserFacts → 20,
// FindCandidates → 5). We assert the default path returns rows (not the no-op
// of a negative bind).
TEST_F(MemoryBranchesTest, NonPositiveLimitsUseDefaults) {
    MemoryBlockAdapter adapter(*store_, nullptr, nullptr);
    auto meta = nlohmann::json{{"memory_type", "fact"}, {"status", "active"},
                               {"user_id", "dave"}, {"created_at", "2026-06-12T10:00:00Z"}};
    ASSERT_TRUE(adapter.InsertMemoryBlock(Rec("01MEMLIM", "dave", "fact one", meta)).ok());

    // limit = 0 → the `limit > 0 ? limit : 20` else (default 20) arm.
    auto facts = QueryUserFacts(*store_, "dave", /*limit=*/0);
    ASSERT_TRUE(facts.ok());
    EXPECT_EQ(facts.value().size(), 1u);

    // top_k = 0 → the `top_k > 0 ? top_k : 5` else (default 5) arm.
    MemoryContradictionAdapter contra(*store_);
    auto cands = contra.FindCandidates("dave", "fact one", /*top_k=*/0);
    EXPECT_EQ(cands.size(), 1u);
}

}  // namespace
}  // namespace cortrix::memory

// ===================================================================
// bloom_filter.cpp ctor edge branches (68% → covers the arg-validation
// arms the existing test_bloom_filter.cpp does not exercise).
// ===================================================================
namespace cortrix::catalog {
namespace {

// capacity_bytes == 0 → the ctor clamps bits to 1 byte (the `capacity_bytes==0`
// ternary arm) and still builds a usable 1-word filter (no div-by-zero).
TEST(BloomFilterCtorBranchesTest, ZeroCapacityStillUsable) {
    BloomFilter bf(/*capacity_bytes=*/0, /*target_fp_rate=*/0.01);
    bf.MarkReady(1);
    ASSERT_TRUE(bf.Add("k").ok());
    EXPECT_TRUE(bf.MightContain("k"));   // no false negative even at minimum size
    EXPECT_EQ(bf.EstimatedCount(), 1u);
}

// target_fp_rate outside (0,1) → the ctor falls back to the default p=0.01 (the
// `target_fp_rate>0 && <1` false arm). Covers 0, 1.0, and a negative.
TEST(BloomFilterCtorBranchesTest, InvalidFpRateFallsBackToDefault) {
    for (double bad_fp : {0.0, 1.0, -0.5, 2.0}) {
        BloomFilter bf(64 * 1024, bad_fp);
        bf.MarkReady(1);
        ASSERT_TRUE(bf.Add("present").ok());
        EXPECT_TRUE(bf.MightContain("present")) << "fp=" << bad_fp;
        // A disjoint key is (almost surely) absent at this fill → the default k
        // produced a working filter, not a degenerate k=0.
        EXPECT_FALSE(bf.MightContain("definitely-absent-xyz")) << "fp=" << bad_fp;
    }
}

// A very loose target (p close to 1) drives k below 1 → clamped to 1 (k<1 arm).
// A working filter with k=1 still has no false negatives for added keys.
TEST(BloomFilterCtorBranchesTest, LooseRateClampsKToOne) {
    BloomFilter bf(64 * 1024, /*target_fp_rate=*/0.99);  // -log2(0.99)≈0.0145 → round 0 → clamp 1
    bf.MarkReady(1);
    for (int i = 0; i < 200; ++i) ASSERT_TRUE(bf.Add("k-" + std::to_string(i)).ok());
    for (int i = 0; i < 200; ++i)
        EXPECT_TRUE(bf.MightContain("k-" + std::to_string(i)));  // no false negatives
}

// A tiny target (p≈0) drives k above 30 → clamped to 30 (k>30 arm). Still no
// false negatives for added keys.
TEST(BloomFilterCtorBranchesTest, TinyRateClampsKToThirty) {
    BloomFilter bf(1024 * 1024, /*target_fp_rate=*/1e-15);  // -log2≈49.8 → clamp 30
    bf.MarkReady(1);
    for (int i = 0; i < 200; ++i) ASSERT_TRUE(bf.Add("k-" + std::to_string(i)).ok());
    for (int i = 0; i < 200; ++i)
        EXPECT_TRUE(bf.MightContain("k-" + std::to_string(i)));
}

}  // namespace
}  // namespace cortrix::catalog

// ===================================================================
// gc/blob_gc_sink.cpp + gc/gc_manager.cpp residual reachable branches
// (the small arms the existing gc tests don't reach).
// ===================================================================
namespace cortrix::catalog::gc {
namespace {

namespace fs = std::filesystem;

int64_t R7NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
int64_t R7DaysAgoMs(int days) {
    return R7NowMs() - static_cast<int64_t>(days) * 24 * 3600 * 1000;
}

// --- blob_gc_sink: the `if (ec)` real-error arm (existing tests cover only
//     success + idempotent-missing). fs::remove on a NON-EMPTY directory fails
//     with an error_code (it only removes empty dirs / files), hitting the
//     CX_ERR_GC_BLOB_UNLINK_FAILED branch — a genuine fs error, no hack. ---
TEST(BlobGcSinkErrorArmTest, RemoveNonEmptyDirSurfacesError) {
    const fs::path base = fs::temp_directory_path() /
        ("gc_sink_err_" + std::to_string(reinterpret_cast<uintptr_t>(&base)));
    fs::create_directories(base / "ab" / "abcd" / "child");  // abcd is a non-empty dir
    { std::ofstream(base / "ab" / "abcd" / "child" / "f") << "x"; }

    LocalBlobGcSink sink(base.string());
    Status s = sink.Unlink("ab/abcd");  // remove a non-empty directory → ec set
    EXPECT_FALSE(s.ok()) << "removing a non-empty dir must surface an fs error";
    EXPECT_NE(s.message().find("CX_ERR_GC_BLOB_UNLINK_FAILED"), std::string::npos);

    std::error_code ec;
    fs::remove_all(base, ec);
}

// gc_manager fixture (mirrors test_gc_manager_extra seed helpers, inline).
class GcManagerResidualTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        cfg_.soft_delete_retention_days = 30;
        cfg_.blob_gc_retention_days = 90;
    }
    void SeedFile(const std::string& fh, const std::string& blob_uri,
                  const std::string& status, int64_t deleted_at) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO file_locations (file_hash, unit_id, ns_id, tenant_id, "
            "size_bytes, blob_uri, ref_count, first_seen_at, status, deleted_at, created_at) "
            "VALUES (?, 'u1','ns1','t1', 10, ?, 0, 0, ?, ?, 0)";
        ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(), sql, -1, &stmt, nullptr), SQLITE_OK);
        sqlite3_bind_text(stmt, 1, fh.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, blob_uri.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, deleted_at);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    int CountRows(const std::string& sql) {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(catalog_.db(), sql.c_str(), -1, &stmt, nullptr), SQLITE_OK);
        int n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return n;
    }
    CatalogDb catalog_;
    GcConfig cfg_;
    NullBlobGcSink sink_;
};

// Stage 2 cap: max_purge_per_run limits Stage 2 hard-deletes (the `done >= cap`
// arm in Stage2HardDelete — existing extra test only exercises the Stage 3 cap).
TEST_F(GcManagerResidualTest, Stage2HardDeleteHitsRunCap) {
    for (int i = 0; i < 3; ++i)
        SeedFile("fh-" + std::to_string(i), "b-" + std::to_string(i), "deleted",
                 R7DaysAgoMs(31));  // all past the 30d retention window
    cfg_.max_purge_per_run = 2;
    GcManager mgr(catalog_.db(), &sink_, cfg_);

    auto r = mgr.RunOnce();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().hard_deleted, 2);
    EXPECT_TRUE(r.value().hit_run_cap);
    EXPECT_EQ(CountRows("SELECT COUNT(*) FROM file_locations"), 1);  // one left
}

// Purge (bypass_windows): a freshly soft-deleted file (inside the retention
// window, so RunOnce would skip it) IS collected by Purge — the Stage2
// bypass_windows cutoff arm + the same-sweep immediate enqueue/unlink path.
TEST_F(GcManagerResidualTest, PurgeBypassesRetentionWindowAndUnlinksSameSweep) {
    SeedFile("fh-fresh", "blob://fresh", "deleted", R7DaysAgoMs(1));  // 1 day < 30d window
    GcManager mgr(catalog_.db(), &sink_, cfg_);

    // RunOnce respects the window → nothing collected yet.
    auto run = mgr.RunOnce();
    ASSERT_TRUE(run.ok());
    EXPECT_EQ(run.value().hard_deleted, 0);
    EXPECT_EQ(CountRows("SELECT COUNT(*) FROM file_locations"), 1);

    // Purge bypasses the window: Stage 2 hard-deletes + enqueues with eligible_at
    // collapsed to NOW, so Stage 3 in the SAME sweep unlinks the blob.
    auto purge = mgr.Purge();
    ASSERT_TRUE(purge.ok()) << purge.status().message();
    EXPECT_EQ(purge.value().hard_deleted, 1);
    EXPECT_EQ(purge.value().blobs_unlinked, 1);  // same-sweep unlink (bypass collapse)
    EXPECT_EQ(CountRows("SELECT COUNT(*) FROM file_locations"), 0);
}

// Restore reactivation: a soft-deleted file with an active content_ref restores
// cleanly (the happy reactivation + ref_count recompute arms). doc-with-no-ref
// fails (CX_ERR_GC_DOC_NOT_FOUND) — both in one call to cover the mixed path the
// existing extra test covers from a different seed shape.
TEST_F(GcManagerResidualTest, RestoreReactivatesContentRef) {
    SeedFile("fh-r", "blob://r", "deleted", R7DaysAgoMs(5));
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(),
        "INSERT INTO content_refs (file_hash, ns_id, doc_id, target_unit_id, status, created_at) "
        "VALUES ('fh-r','ns1','doc-r','','deleted',0)", -1, &st, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);

    GcManager mgr(catalog_.db(), &sink_, cfg_);
    auto r = mgr.Restore({"doc-r"});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().succeeded.size(), 1u);
    EXPECT_EQ(r.value().succeeded[0], "doc-r");
    // File flipped back to active with ref_count recomputed from the now-active ref.
    EXPECT_EQ(CountRows("SELECT COUNT(*) FROM file_locations WHERE status='active'"), 1);
    EXPECT_EQ(CountRows("SELECT ref_count FROM file_locations WHERE file_hash='fh-r'"), 1);
}

}  // namespace
}  // namespace cortrix::catalog::gc
