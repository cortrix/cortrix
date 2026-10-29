#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/resource/namespace_resource_bundle.h"
#include "cortrix/resource/pool_error.h"
#include "ns_pool_test_helper.h"

// Extra deterministic acquire/release/evict/idempotency edges for the F05
// DefaultNamespacePool, built on the shared NsPoolHarness (real WriteCoordinator
// over a temp dir + mocked F12 routers). The basic admission/budget/startup paths
// live in test_namespace_pool*.cpp; this adds the resident-lifecycle edges in a
// NEW unique fixture (NsPoolExtra) so there is no suite-name clash.

namespace cortrix {
namespace {

class NsPoolExtra : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dir = std::filesystem::temp_directory_path() /
               ("nspool_extra_" +
                std::to_string(::testing::UnitTest::GetInstance()
                                   ->current_test_info()
                                   ->line()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    harness_ = std::make_unique<cortrix::test::NsPoolHarness>(dir);
  }

  std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
};

// Acquire an unknown NS -> CX_ERR_NS_NOT_FOUND token + kNotFound code.
TEST_F(NsPoolExtra, AcquireUnknownIsNotFoundWithToken) {
  auto& pool = harness_->pool();
  Result<resource::NamespaceResourceBundle*> acq = pool.Acquire("ghost");
  ASSERT_FALSE(acq.ok());
  EXPECT_EQ(acq.status().code(), StatusCode::kNotFound);
  EXPECT_NE(acq.status().message().find("CX_ERR_NS_NOT_FOUND"), std::string::npos);
}

// Idempotent re-admit of a resident NS returns Ok and does not grow the pool.
TEST_F(NsPoolExtra, ReAdmitResidentIsIdempotentNoGrowth) {
  ASSERT_TRUE(harness_->Admit("ns-a", 100).ok());
  const size_t size_after_first = harness_->pool().GetPoolStats().pool_size_current;
  EXPECT_EQ(size_after_first, 1u);
  // Second admit of the same NS: Ok, size unchanged.
  Status s = harness_->pool().AdmitCreate("ns-a", 999);
  EXPECT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(harness_->pool().GetPoolStats().pool_size_current, 1u);
}

// Acquire + Release pair, then re-acquire succeeds (refcount returns to 0).
TEST_F(NsPoolExtra, AcquireReleaseReacquire) {
  ASSERT_TRUE(harness_->Admit("ns-b", 100).ok());
  auto& pool = harness_->pool();

  Result<resource::NamespaceResourceBundle*> a1 = pool.Acquire("ns-b");
  ASSERT_TRUE(a1.ok());
  ASSERT_NE(a1.value(), nullptr);
  EXPECT_EQ(a1.value()->namespace_id, "ns-b");
  pool.Release("ns-b");

  Result<resource::NamespaceResourceBundle*> a2 = pool.Acquire("ns-b");
  ASSERT_TRUE(a2.ok());
  EXPECT_EQ(a2.value(), a1.value());  // same node-stable slot address
  pool.Release("ns-b");
}

// Release on a non-resident NS is a safe no-op (no crash, idempotent).
TEST_F(NsPoolExtra, ReleaseUnknownIsNoOp) {
  harness_->pool().Release("never-existed");  // must not crash
  harness_->pool().Release("never-existed");
  SUCCEED();
}

// EvictForDelete with no outstanding borrow drops the NS; later Acquire fails.
TEST_F(NsPoolExtra, EvictThenAcquireFails) {
  ASSERT_TRUE(harness_->Admit("ns-c", 100).ok());
  auto& pool = harness_->pool();
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 1u);

  Status ev = pool.EvictForDelete("ns-c");
  EXPECT_TRUE(ev.ok()) << ev.message();
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 0u);

  Result<resource::NamespaceResourceBundle*> acq = pool.Acquire("ns-c");
  EXPECT_FALSE(acq.ok());
  EXPECT_EQ(acq.status().code(), StatusCode::kNotFound);
}

// EvictForDelete is idempotent: a second evict of an absent NS returns Ok.
TEST_F(NsPoolExtra, EvictIsIdempotent) {
  ASSERT_TRUE(harness_->Admit("ns-d", 100).ok());
  EXPECT_TRUE(harness_->pool().EvictForDelete("ns-d").ok());
  EXPECT_TRUE(harness_->pool().EvictForDelete("ns-d").ok());  // absent -> Ok
  EXPECT_TRUE(harness_->pool().EvictForDelete("never").ok());
}

// EvictForDelete deferred while a borrow is outstanding: the last Release reaps.
TEST_F(NsPoolExtra, EvictDeferredUntilLastRelease) {
  ASSERT_TRUE(harness_->Admit("ns-e", 100).ok());
  auto& pool = harness_->pool();

  Result<resource::NamespaceResourceBundle*> borrow = pool.Acquire("ns-e");
  ASSERT_TRUE(borrow.ok());

  // Evict while borrowed: marked pending_delete, NOT yet reaped.
  EXPECT_TRUE(pool.EvictForDelete("ns-e").ok());
  // A new Acquire is refused once pending_delete is set.
  Result<resource::NamespaceResourceBundle*> mid = pool.Acquire("ns-e");
  EXPECT_FALSE(mid.ok());
  EXPECT_EQ(mid.status().code(), StatusCode::kNotFound);

  // Last Release reaps the slot.
  pool.Release("ns-e");
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 0u);
}

// Multiple distinct namespaces are independently resident.
TEST_F(NsPoolExtra, MultipleNamespacesIndependent) {
  ASSERT_TRUE(harness_->Admit("ns-1", 100).ok());
  ASSERT_TRUE(harness_->Admit("ns-2", 100).ok());
  ASSERT_TRUE(harness_->Admit("ns-3", 100).ok());
  auto& pool = harness_->pool();
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 3u);

  EXPECT_TRUE(pool.Acquire("ns-1").ok());
  pool.Release("ns-1");
  EXPECT_TRUE(pool.EvictForDelete("ns-2").ok());
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 2u);
  EXPECT_TRUE(pool.Acquire("ns-3").ok());
  pool.Release("ns-3");
}

// A crash can leave documents 'processing' (to be ingested again) or 'deleting'
// with their blocks and vectors on disk. Loading the namespace drops those
// vectors (block ids and contextual labels) before the doc recovery deletes the
// blocks, so re-ingest does not leave the old ones behind; a ready document's
// vectors, label rows and parents are left alone.
TEST_F(NsPoolExtra, LoadDropsVectorsOfCrashedDocuments) {
  ASSERT_TRUE(harness_->Admit("ns-crash", 100).ok());
  auto& pool = harness_->pool();
  {
    Result<resource::NamespaceResourceBundle*> acq = pool.Acquire("ns-crash");
    ASSERT_TRUE(acq.ok());
    sqlite3* db = acq.value()->primary().store_db->handle();
    const char* seed =
        "INSERT INTO documents(doc_id, source_type, source_path, status) VALUES"
        " ('doc-processing', 'file', 'a.md', 'processing'),"
        " ('doc-deleting', 'file', 'b.md', 'deleting'),"
        " ('doc-ready', 'file', 'c.md', 'ready');"
        "INSERT INTO blocks(block_id, doc_id, chunk_index, block_type,"
        " processing_level, data) VALUES"
        " (11, 'doc-processing', 0, 1, 3, x'00'),"
        " (12, 'doc-processing', 1, 1, 3, x'00'),"
        " (21, 'doc-deleting', 0, 1, 3, x'00'),"
        " (31, 'doc-ready', 0, 1, 3, x'00');"
        "INSERT INTO contextual_vec_labels(label, block_id, child_id) VALUES"
        " (111, 11, 'child-11'), (311, 31, 'child-31');"
        "INSERT INTO parents(parent_id, doc_id, namespace_id, parent_text, token_count,"
        " page_start, page_end, byte_offset_start, byte_offset_end, metadata_json,"
        " created_at) VALUES"
        " ('parent-p', 'doc-processing', 'ns-crash', 't', 1, 0, 0, 0, 1, '{}', 0),"
        " ('parent-r', 'doc-ready', 'ns-crash', 't', 1, 0, 0, 0, 1, '{}', 0);";
    ASSERT_EQ(sqlite3_exec(db, seed, nullptr, nullptr, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    pool.Release("ns-crash");
  }

  // Restart: drop the resident namespace and load it again from disk.
  ASSERT_TRUE(pool.EvictForDelete("ns-crash").ok());
  ASSERT_TRUE(pool.AdmitCreate("ns-crash", 100).ok());

  ASSERT_NE(harness_->fake_index(), nullptr);
  std::vector<uint64_t> dropped = harness_->fake_index()->deleted_ids();
  std::sort(dropped.begin(), dropped.end());
  EXPECT_EQ(dropped, (std::vector<uint64_t>{11, 12, 21, 111}));

  Result<resource::NamespaceResourceBundle*> acq = pool.Acquire("ns-crash");
  ASSERT_TRUE(acq.ok());
  sqlite3* db = acq.value()->primary().store_db->handle();
  auto count = [db](const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    const int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return n;
  };
  EXPECT_EQ(count("SELECT count(*) FROM contextual_vec_labels WHERE label = 111"), 0);
  EXPECT_EQ(count("SELECT count(*) FROM contextual_vec_labels WHERE label = 311"), 1);
  EXPECT_EQ(count("SELECT count(*) FROM parents WHERE doc_id = 'doc-processing'"), 0);
  EXPECT_EQ(count("SELECT count(*) FROM parents WHERE doc_id = 'doc-ready'"), 1);
  // The doc recovery itself still ran after the vectors were dropped.
  EXPECT_EQ(count("SELECT count(*) FROM blocks WHERE doc_id = 'doc-processing'"), 0);
  EXPECT_EQ(count("SELECT count(*) FROM documents"
                  " WHERE doc_id = 'doc-processing' AND status = 'pending'"),
            1);
  EXPECT_EQ(count("SELECT count(*) FROM documents WHERE doc_id = 'doc-deleting'"), 0);
  EXPECT_EQ(count("SELECT count(*) FROM blocks WHERE doc_id = 'doc-ready'"), 1);
  pool.Release("ns-crash");
}

namespace {
int CountRows(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
  const int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return n;
}
}  // namespace

// A crash in the middle of an ingest leaves a PENDING txn, which the WC Recover
// rolls back on the next load. The ingest writes its contextual label rows right
// after BeginWrite, keyed by the txn's block ids, so the rollback finds the
// contextual points through them whether no block row was written yet or only
// some were.
TEST_F(NsPoolExtra, LoadRollsBackContextualPointsOfPendingIngest) {
  for (const bool partial : {false, true}) {
    SCOPED_TRACE(partial ? "some block rows written" : "no block row written");
    const std::string ns = partial ? "ns-pending-partial" : "ns-pending-none";
    ASSERT_TRUE(harness_->Admit(ns, 100).ok());
    auto& pool = harness_->pool();
    {
      Result<resource::NamespaceResourceBundle*> acq = pool.Acquire(ns);
      ASSERT_TRUE(acq.ok());
      auto& unit = acq.value()->primary();
      sqlite3* db = unit.store_db->handle();
      ASSERT_EQ(sqlite3_exec(db,
                             "INSERT INTO documents(doc_id, source_type, source_path, status)"
                             " VALUES ('doc-a', 'file', 'a.md', 'processing');",
                             nullptr, nullptr, nullptr),
                SQLITE_OK)
          << sqlite3_errmsg(db);
      store::TxnHandle txn;
      ASSERT_TRUE(unit.pwl->BeginWrite("doc-a", {41, 42}, /*writes_blob=*/false, &txn).ok());
      std::string seed =
          "INSERT INTO contextual_vec_labels(label, block_id, child_id) VALUES"
          " (141, 41, 'child-41'), (142, 42, 'child-42');";
      if (partial) {
        seed +=
            "INSERT INTO blocks(block_id, doc_id, chunk_index, block_type,"
            " processing_level, data) VALUES (41, 'doc-a', 0, 1, 3, x'00');";
      }
      ASSERT_EQ(sqlite3_exec(db, seed.c_str(), nullptr, nullptr, nullptr), SQLITE_OK)
          << sqlite3_errmsg(db);
      // No Commit: the process dies here.
      pool.Release(ns);
    }

    ASSERT_TRUE(pool.EvictForDelete(ns).ok());
    ASSERT_TRUE(pool.AdmitCreate(ns, 100).ok());

    ASSERT_NE(harness_->fake_index(), nullptr);
    std::vector<uint64_t> dropped = harness_->fake_index()->deleted_ids();
    std::sort(dropped.begin(), dropped.end());
    EXPECT_EQ(dropped, (std::vector<uint64_t>{41, 42, 141, 142}));

    Result<resource::NamespaceResourceBundle*> acq = pool.Acquire(ns);
    ASSERT_TRUE(acq.ok());
    sqlite3* db = acq.value()->primary().store_db->handle();
    EXPECT_EQ(CountRows(db, "SELECT count(*) FROM contextual_vec_labels"), 0);
    EXPECT_EQ(CountRows(db, "SELECT count(*) FROM blocks WHERE doc_id = 'doc-a'"), 0);
    pool.Release(ns);
  }
}

// A label row whose block is gone can never resolve again: a rollback that ran
// before the rollback dropped contextual points left it, or a hard delete, which
// removes blocks but not their contextual points. The load drops the point and
// the row, and leaves the rows of blocks that are still there.
TEST_F(NsPoolExtra, LoadDropsContextualPointsWhoseBlockIsGone) {
  ASSERT_TRUE(harness_->Admit("ns-stale", 100).ok());
  auto& pool = harness_->pool();
  {
    Result<resource::NamespaceResourceBundle*> acq = pool.Acquire("ns-stale");
    ASSERT_TRUE(acq.ok());
    sqlite3* db = acq.value()->primary().store_db->handle();
    const char* seed =
        "INSERT INTO documents(doc_id, source_type, source_path, status) VALUES"
        " ('doc-ready', 'file', 'c.md', 'ready');"
        "INSERT INTO blocks(block_id, doc_id, chunk_index, block_type,"
        " processing_level, data) VALUES (31, 'doc-ready', 0, 1, 3, x'00');"
        "INSERT INTO contextual_vec_labels(label, block_id, child_id) VALUES"
        " (311, 31, 'child-31'), (511, 51, 'child-51');";
    ASSERT_EQ(sqlite3_exec(db, seed, nullptr, nullptr, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    pool.Release("ns-stale");
  }

  ASSERT_TRUE(pool.EvictForDelete("ns-stale").ok());
  ASSERT_TRUE(pool.AdmitCreate("ns-stale", 100).ok());

  ASSERT_NE(harness_->fake_index(), nullptr);
  EXPECT_EQ(harness_->fake_index()->deleted_ids(), (std::vector<uint64_t>{511}));

  Result<resource::NamespaceResourceBundle*> acq = pool.Acquire("ns-stale");
  ASSERT_TRUE(acq.ok());
  sqlite3* db = acq.value()->primary().store_db->handle();
  EXPECT_EQ(CountRows(db, "SELECT count(*) FROM contextual_vec_labels WHERE label = 511"), 0);
  EXPECT_EQ(CountRows(db, "SELECT count(*) FROM contextual_vec_labels WHERE label = 311"), 1);
  pool.Release("ns-stale");
}

}  // namespace
}  // namespace cortrix
