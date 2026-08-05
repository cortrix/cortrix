#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/catalog/i_unit_router.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/resource/namespace_pool_config.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/resource/namespace_resource_bundle.h"
#include "cortrix/resource/pool_error.h"
#include "cortrix/store/iindex.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/write_coordinator.h"

// Additional branch coverage for DefaultNamespacePool::LoadOneNamespaceInner —
// the per-NS load failure sub-paths that test_namespace_pool.cpp does not reach
// standalone: GetActiveUnit failure, the index→IVectorStore cross-cast null
// branch, the WriteCoordinator factory failure, the store.db open failure, and
// the UnitDataDir layout branches (empty / trailing-slash data_root).
namespace cortrix::resource {
namespace {

using ::testing::_;
using ::testing::Invoke;
using cortrix::store::IIndex;
using cortrix::store::IIndexFactory;
using cortrix::store::IndexConfig;
using cortrix::store::IndexStats;
using cortrix::store::WriteCoordinator;
using cortrix::store::WriteCoordinatorConfig;

// An IIndex that does NOT also implement IVectorStore. The pool cross-casts
// bundle.index to IVectorStore* (D3.5 C1 §9.3); against this index the
// dynamic_cast returns null → the "index_not_ivector_store" load-failure branch.
class NonVectorIndex : public IIndex {
public:
    Status AddPoint(const float*, uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>&,
                     const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status MarkDelete(uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    std::vector<std::pair<uint64_t, float>> Search(const float*, int, int,
                                                   const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t) override { return false; }
    Status Snapshot() override { return Status::Ok(); }
    Status Recover() override { return Status::Ok(); }
    Status Shutdown() override { return Status::Ok(); }
    IndexStats GetStats() override { return IndexStats{}; }
    std::size_t GetMemoryFootprintBytes() const override { return 0; }
};

// A normal IIndex + IVectorStore fake (matches the production PHnsw shape).
class DualIndex : public IIndex, public cortrix::store::IVectorStore {
public:
    Status AddPoint(const float*, uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status MarkDelete(uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>&,
                     const observability::TraceContext*) override {
        return Status::Ok();
    }
    std::vector<std::pair<uint64_t, float>> Search(const float*, int, int,
                                                   const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t) override { return false; }
    Status Snapshot() override { return Status::Ok(); }
    Status Recover() override { return Status::Ok(); }
    Status Shutdown() override { return Status::Ok(); }
    IndexStats GetStats() override { return IndexStats{}; }
    std::size_t GetMemoryFootprintBytes() const override { return 0; }
    std::vector<std::pair<uint64_t, float>> Search(const float*, int,
                                                   const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t, const observability::TraceContext*) override { return false; }
    Status Snapshot(const observability::TraceContext*) override { return Status::Ok(); }
    Status Recover(const observability::TraceContext*) override { return Status::Ok(); }
};

class MockIndexFactory : public IIndexFactory {
public:
    MOCK_METHOD((Result<std::unique_ptr<IIndex>>), Create,
                (const std::string&, const IndexConfig&), (override));
    MOCK_METHOD((Result<std::unique_ptr<IIndex>>), Open, (const std::string&), (override));
};

class MockNSRouter : public cortrix::catalog::INSRouter {
public:
    MOCK_METHOD((Result<std::vector<cortrix::catalog::UnitDescriptor>>), LookupUnits,
                (const std::string&), (const, override));
    MOCK_METHOD((Result<cortrix::catalog::UnitDescriptor>), GetActiveUnit,
                (const std::string&), (const, override));
    MOCK_METHOD((Result<cortrix::catalog::NSMetadata>), GetNamespace,
                (const std::string&), (const, override));
    MOCK_METHOD((Result<cortrix::catalog::BatchResult<std::string>>), ListNamespaces,
                (const cortrix::catalog::ListNamespacesOptions&), (const, override));
    MOCK_METHOD(Status, CreateNamespace, (const cortrix::catalog::NSMetadata&), (override));
    MOCK_METHOD(Status, DeleteNamespace, (const std::string&), (override));
};

class MockUnitRouter : public cortrix::catalog::IUnitRouter {
public:
    MOCK_METHOD((Result<cortrix::catalog::UnitDescriptor>), GetUnit,
                (const std::string&), (const, override));
    MOCK_METHOD((Result<cortrix::catalog::UnitState>), GetUnitState,
                (const std::string&), (const, override));
    MOCK_METHOD(Status, UpdateUnitState,
                (const std::string&, cortrix::catalog::UnitState), (override));
    MOCK_METHOD(Status, UpdateUnitStats,
                (const std::string&, const cortrix::catalog::UnitStats&), (override));
    MOCK_METHOD((Result<bool>), ShouldSeal, (const std::string&), (const, override));
};

class NsPoolBranchTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_root_ = std::filesystem::temp_directory_path() /
                    ("f05_pool_branch_" +
                     std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(tmp_root_);
        config_.data_root = tmp_root_.string();
        config_.load_timeout_ms_per_ns = 0;  // run LoadOneNamespaceInner inline
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmp_root_, ec);
    }

    cortrix::catalog::UnitDescriptor Unit(const std::string& unit_id) {
        cortrix::catalog::UnitDescriptor u;
        u.unit_id = unit_id;
        u.tenant_id = "t1";
        return u;
    }
    void MakeUnitDir(const std::string& unit_id) {
        std::filesystem::create_directories(tmp_root_ / unit_id);
    }
    void StubActiveUnit() {
        ON_CALL(ns_router_, GetActiveUnit(_))
            .WillByDefault(Invoke([this](const std::string& ns) {
                return Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
            }));
    }
    // The real-WriteCoordinator factory (succeeds over a temp dir).
    WriteCoordinatorFactory RealCoordFactory() {
        return [](const std::string& dir, const WriteCoordinatorConfig& cfg,
                  cortrix::store::IVectorStore* vs, cortrix::store::IMetadataStore* ms,
                  cortrix::store::IBlobStore* bs)
                   -> Result<std::unique_ptr<WriteCoordinator>> {
            auto coord = std::make_unique<WriteCoordinator>(dir, cfg, vs, ms, bs);
            Status init = coord->Init();
            if (!init.ok()) return init;
            return Result<std::unique_ptr<WriteCoordinator>>(std::move(coord));
        };
    }
    // A factory that always fails (the open_pwl load-failure branch).
    WriteCoordinatorFactory FailingCoordFactory() {
        return [](const std::string&, const WriteCoordinatorConfig&,
                  cortrix::store::IVectorStore*, cortrix::store::IMetadataStore*,
                  cortrix::store::IBlobStore*)
                   -> Result<std::unique_ptr<WriteCoordinator>> {
            return Status(StatusCode::kInternal, "forced coord init failure");
        };
    }

    std::filesystem::path tmp_root_;
    NamespacePoolConfig config_;
    ::testing::NiceMock<MockIndexFactory> index_factory_;
    ::testing::NiceMock<MockNSRouter> ns_router_;
    ::testing::NiceMock<MockUnitRouter> unit_router_;
};

// GetActiveUnit failure → the "get_active_unit" load-failure branch (step 1).
TEST_F(NsPoolBranchTest, LoadFailsWhenActiveUnitLookupFails) {
    ON_CALL(ns_router_, GetActiveUnit(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<cortrix::catalog::UnitDescriptor>(
                Status(StatusCode::kNotFound, "CX_ERR_NS_NOT_FOUND: gone"));
        }));
    DefaultNamespacePool pool(&index_factory_, RealCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    Status s = pool.AdmitCreate("ns-no-unit", 100);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_NS_LOAD_FAILED"), std::string::npos);
    EXPECT_NE(s.message().find("get_active_unit"), std::string::npos);
}

// The index opens but is not an IVectorStore → the cross-cast null branch.
TEST_F(NsPoolBranchTest, LoadFailsWhenIndexIsNotIVectorStore) {
    StubActiveUnit();
    MakeUnitDir("ns-nv-u");
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(std::make_unique<NonVectorIndex>());
        }));
    DefaultNamespacePool pool(&index_factory_, RealCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    Status s = pool.AdmitCreate("ns-nv", 100);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_NS_LOAD_FAILED"), std::string::npos);
    EXPECT_NE(s.message().find("index_not_ivector_store"), std::string::npos);
}

// The WriteCoordinator factory fails → the "open_pwl" load-failure branch.
TEST_F(NsPoolBranchTest, LoadFailsWhenCoordFactoryFails) {
    StubActiveUnit();
    MakeUnitDir("ns-wc-u");
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(std::make_unique<DualIndex>());
        }));
    DefaultNamespacePool pool(&index_factory_, FailingCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    Status s = pool.AdmitCreate("ns-wc", 100);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_NS_LOAD_FAILED"), std::string::npos);
    EXPECT_NE(s.message().find("open_pwl"), std::string::npos);
}

// The Unit dir is absent → SqliteConn::Open of store.db fails → the
// "open_store_db" load-failure branch (step 3).
TEST_F(NsPoolBranchTest, LoadFailsWhenStoreDbCannotOpen) {
    StubActiveUnit();
    // Deliberately do NOT create the unit dir; store.db's parent path is missing.
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(std::make_unique<DualIndex>());
        }));
    // Point data_root at a path with no on-disk unit subdir so Open(store.db) fails.
    config_.data_root = (tmp_root_ / "missing-parent").string();
    DefaultNamespacePool pool(&index_factory_, RealCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    Status s = pool.AdmitCreate("ns-nodir", 100);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_NS_LOAD_FAILED"), std::string::npos);
    EXPECT_NE(s.message().find("open_store_db"), std::string::npos);
}

// data_root with a trailing slash exercises the UnitDataDir trailing-'/' branch;
// the load still succeeds and the NS becomes resident.
TEST_F(NsPoolBranchTest, TrailingSlashDataRootLoadsSuccessfully) {
    StubActiveUnit();
    config_.data_root = tmp_root_.string() + "/";  // trailing slash branch
    std::filesystem::create_directories(tmp_root_ / "ns-ts-u");
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(std::make_unique<DualIndex>());
        }));
    DefaultNamespacePool pool(&index_factory_, RealCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    ASSERT_TRUE(pool.AdmitCreate("ns-ts", 100).ok());
    EXPECT_TRUE(pool.Acquire("ns-ts").ok());
}

// Re-admitting an already-resident NS is the idempotent early-return branch
// (no second load, pool size unchanged).
TEST_F(NsPoolBranchTest, ReAdmitResidentNamespaceIsIdempotent) {
    StubActiveUnit();
    MakeUnitDir("ns-idem-u");
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(std::make_unique<DualIndex>());
        }));
    DefaultNamespacePool pool(&index_factory_, RealCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    ASSERT_TRUE(pool.AdmitCreate("ns-idem", 100).ok());
    EXPECT_EQ(pool.GetPoolStats().pool_size_current, 1u);
    // Second admit of the same NS short-circuits (already_resident) → still Ok.
    ASSERT_TRUE(pool.AdmitCreate("ns-idem", 100).ok());
    EXPECT_EQ(pool.GetPoolStats().pool_size_current, 1u);
}

// An already-resident NS does not count against the memory budget gate (the
// already_resident bypass of Gate 2): re-admit succeeds even with budget enabled
// and at capacity.
TEST_F(NsPoolBranchTest, ResidentReAdmitBypassesMemoryGate) {
    StubActiveUnit();
    config_.memory_budget_bytes = 1000;  // enable the budget gate
    MakeUnitDir("ns-mb-u");
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(std::make_unique<DualIndex>());
        }));
    DefaultNamespacePool pool(&index_factory_, RealCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    ASSERT_TRUE(pool.AdmitCreate("ns-mb", 500).ok());
    // Re-admit with a huge estimate: resident bypass means the gate is skipped.
    ASSERT_TRUE(pool.AdmitCreate("ns-mb", 1u << 30).ok());
    EXPECT_EQ(pool.GetPoolStats().pool_size_current, 1u);
}

// The step-8c memory.db pre-warm failure is TOLERATED: make <unit_dir>/memory.db a
// directory before load so MemoryStore::Init() fails during assembly → the WARN
// branch runs, but the NS still loads successfully (degraded, not fatal).
TEST_F(NsPoolBranchTest, MemoryPrewarmFailureIsToleratedDuringLoad) {
    StubActiveUnit();
    MakeUnitDir("ns-mpw-u");
    // Pre-place a directory where the pre-warm expects to open memory.db.
    std::filesystem::create_directory(tmp_root_ / "ns-mpw-u" / "memory.db");
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(std::make_unique<DualIndex>());
        }));
    DefaultNamespacePool pool(&index_factory_, RealCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    // Load tolerates the pre-warm failure → admit still succeeds, NS resident.
    ASSERT_TRUE(pool.AdmitCreate("ns-mpw", 100).ok());
    EXPECT_TRUE(pool.Acquire("ns-mpw").ok());
}

// EvictForDelete on a resident NS releases it and refreshes the size gauge; a
// second evict of the now-absent NS is the idempotent erase-absent branch.
TEST_F(NsPoolBranchTest, EvictForDeleteThenIdempotentSecondEvict) {
    StubActiveUnit();
    MakeUnitDir("ns-ev-u");
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(std::make_unique<DualIndex>());
        }));
    DefaultNamespacePool pool(&index_factory_, RealCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    ASSERT_TRUE(pool.AdmitCreate("ns-ev", 100).ok());
    ASSERT_TRUE(pool.EvictForDelete("ns-ev").ok());
    EXPECT_EQ(pool.GetPoolStats().pool_size_current, 0u);
    // Second evict: bundles_.erase on an absent key → still Ok (idempotent).
    ASSERT_TRUE(pool.EvictForDelete("ns-ev").ok());
}

// ReloadNamespace's startup-failed-list scrub iterates the whole list, taking BOTH
// the erase arm (the reloaded NS) AND the keep/advance arm (a DIFFERENT still-failed
// NS). The single-failure case exercises only the erase arm.
TEST_F(NsPoolBranchTest, ReloadScrubVisitsMatchingAndNonMatchingFailures) {
    config_.load_timeout_ms_per_ns = 5000;
    MakeUnitDir("good-u");
    MakeUnitDir("bad-1-u");
    MakeUnitDir("bad-2-u");
    StubActiveUnit();
    // Startup list of three; good loads, bad-1/bad-2 fail their index Open.
    ON_CALL(ns_router_, ListNamespaces(_))
        .WillByDefault(Invoke([](const cortrix::catalog::ListNamespacesOptions&) {
            cortrix::catalog::BatchResult<std::string> br;
            br.results = {"good", "bad-1", "bad-2"};
            return Result<cortrix::catalog::BatchResult<std::string>>(br);
        }));
    int good_open_attempts = 0;
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([&good_open_attempts](const std::string& unit_dir) {
            const bool is_bad = unit_dir.find("bad-1") != std::string::npos ||
                                unit_dir.find("bad-2") != std::string::npos;
            if (is_bad) {
                return Result<std::unique_ptr<IIndex>>(
                    Status(StatusCode::kUnavailable, "forced startup failure"));
            }
            ++good_open_attempts;
            return Result<std::unique_ptr<IIndex>>(std::make_unique<DualIndex>());
        }));
    DefaultNamespacePool pool(&index_factory_, RealCoordFactory(), &ns_router_,
                              &unit_router_, config_);

    auto rep = pool.StartupLoadAll();
    ASSERT_TRUE(rep.ok());
    EXPECT_EQ(rep.value().failed, 2u);
    EXPECT_EQ(pool.GetExplainState().startup_failed_namespaces.size(), 2u);

    // Make bad-1 loadable now, reload it: the scrub loop visits [bad-1, bad-2] —
    // bad-1 matches (erase arm), bad-2 does not (advance arm).
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(std::make_unique<DualIndex>());
        }));
    ASSERT_TRUE(pool.ReloadNamespace("bad-1").ok());
    auto failed = pool.GetExplainState().startup_failed_namespaces;
    ASSERT_EQ(failed.size(), 1u) << "only bad-1 should be scrubbed out";
    EXPECT_EQ(failed[0], "bad-2");
}

}  // namespace
}  // namespace cortrix::resource
