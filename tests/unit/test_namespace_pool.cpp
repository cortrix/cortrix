#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/catalog/i_unit_router.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/resource/f05_config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/resource/namespace_resource_bundle.h"
#include "cortrix/resource/pool_error.h"
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/iindex.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/recover_stores.h"
#include "cortrix/store/write_coordinator.h"

// F05 S1 coverage: NamespaceResourceBundle / UnitResourceBundle (UT1, UT2),
// F05Config defaults (UT3), AdmitCreate admission control + rejection log
// (UT4-UT8), Acquire (UT13, UT14), PoolStats / ExplainState surfaces (UT18), and
// the PoolError registry (the §8.1 4-code identity set).
//
// Standalone (D3): the pool is exercised against mocked F12 routers + a real
// F01 PhnswIndexFactory-style FakeIndex + a real F25 WriteCoordinator over temp
// dirs. No cross-Feature wiring (no real INSRouter / IGlobalConfig hook) — those
// are D3.5.
namespace cortrix::resource {
namespace {

using ::testing::_;
using ::testing::Return;
using cortrix::store::IIndex;
using cortrix::store::IIndexFactory;
using cortrix::store::IndexConfig;
using cortrix::store::IndexStats;
using cortrix::store::WriteCoordinator;
using cortrix::store::WriteCoordinatorConfig;

// ── test doubles ─────────────────────────────────────────────────────────────

// IIndex stand-in with a configurable resident footprint so the memory-budget
// gate (UT5/UT6) is deterministic without a real P-HNSW graph.
// Implements BOTH IIndex and IVectorStore — like the real PHnsw (i_vector_store.h
// "PHnsw inherits both"). The pool cross-casts bundle.index to IVectorStore* for
// the WriteCoordinator (D3.5 C1 §9.3), so the fake the index factory returns must
// satisfy both surfaces or the dynamic_cast returns null and the load fails.
// AddPoint/MarkDelete have identical signatures across the two bases → one
// override covers both; Search/Exists/Snapshot/Recover differ (ef_search arg /
// TraceContext arg) → the IVectorStore-shaped overloads are added alongside.
class FakeIndex : public IIndex, public cortrix::store::IVectorStore {
public:
    explicit FakeIndex(std::size_t footprint = 0) : footprint_(footprint) {}
    // —— shared by IIndex + IVectorStore (identical signatures) ——
    Status AddPoint(const float*, uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status MarkDelete(uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    // —— IIndex-only ——
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
    std::size_t GetMemoryFootprintBytes() const override { return footprint_; }
    // —— IVectorStore-only (no ef_search / ctx-carrying variants) ——
    std::vector<std::pair<uint64_t, float>> Search(const float*, int,
                                                   const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t, const observability::TraceContext*) override { return false; }
    Status Snapshot(const observability::TraceContext*) override { return Status::Ok(); }
    Status Recover(const observability::TraceContext*) override { return Status::Ok(); }

private:
    std::size_t footprint_;
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

// Trivial stores so a real WriteCoordinator::Recover() (which requires three
// non-null stores) succeeds against an empty pending.wal.
class FakeVectorStore : public cortrix::store::IVectorStore {
public:
    Status AddPoint(const float*, uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status MarkDelete(uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    std::vector<std::pair<uint64_t, float>> Search(const float*, int,
                                                   const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t, const observability::TraceContext*) override { return false; }
    Status Snapshot(const observability::TraceContext*) override { return Status::Ok(); }
    Status Recover(const observability::TraceContext*) override { return Status::Ok(); }
};
class FakeMetadataStore : public cortrix::store::IMetadataStore {
public:
    bool BlockExists(const std::vector<cortrix::BlockId>&) override { return false; }
};
class FakeBlobStore : public cortrix::store::IBlobStore {
public:
    bool BlobExists(const std::string&) override { return false; }
};

// ── fixture ──────────────────────────────────────────────────────────────────

class NamespacePoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_root_ = std::filesystem::temp_directory_path() /
                    ("f05_pool_" + std::to_string(::testing::UnitTest::GetInstance()
                                                       ->random_seed()) +
                     "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(tmp_root_);
        config_.data_root = tmp_root_.string();
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmp_root_, ec);
    }

    // A UnitDescriptor whose unit_id drives the per-Unit temp dir layout.
    cortrix::catalog::UnitDescriptor Unit(const std::string& unit_id) {
        cortrix::catalog::UnitDescriptor u;
        u.unit_id = unit_id;
        u.tenant_id = "t1";
        return u;
    }

    // Pre-create <data_root>/<unit_id>/ so SqliteConn::Open can create store.db
    // and the WriteCoordinator can create pending.wal inside it.
    void MakeUnitDir(const std::string& unit_id) {
        std::filesystem::create_directories(tmp_root_ / unit_id);
    }

    // Factory building a real WriteCoordinator (Init'd) over the per-NS dir, with
    // the three fake stores so Recover() succeeds on an empty log.
    WriteCoordinatorFactory MakeRealCoordFactory() {
        // D3.5 C1: the pool now resolves the three stores (vector = the Unit's
        // index cross-cast to IVectorStore; metadata + blob = the pool's recover
        // adapters) and passes them in. The factory just news + Init()s the WC
        // over them — it no longer reaches into the fixture's standalone fakes.
        return [](const std::string& data_dir, const WriteCoordinatorConfig& cfg,
                  cortrix::store::IVectorStore* vs, cortrix::store::IMetadataStore* ms,
                  cortrix::store::IBlobStore* bs)
                   -> Result<std::unique_ptr<WriteCoordinator>> {
            auto coord = std::make_unique<WriteCoordinator>(data_dir, cfg, vs, ms, bs);
            Status init = coord->Init();
            if (!init.ok()) return init;
            return Result<std::unique_ptr<WriteCoordinator>>(std::move(coord));
        };
    }

    // Default factory wiring: GetActiveUnit returns Unit(ns+"-u"), the index
    // factory Opens a FakeIndex of `footprint` bytes, the coordinator is real.
    std::unique_ptr<DefaultNamespacePool> MakePool(std::size_t index_footprint = 0) {
        using ::testing::Invoke;
        ON_CALL(ns_router_, GetActiveUnit(_))
            .WillByDefault(Invoke([this](const std::string& ns) {
                return Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
            }));
        ON_CALL(index_factory_, Open(_))
            .WillByDefault(Invoke([index_footprint](const std::string&) {
                return Result<std::unique_ptr<IIndex>>(
                    std::make_unique<FakeIndex>(index_footprint));
            }));
        // Ensure the unit dir exists on first admit of each NS.
        return std::make_unique<DefaultNamespacePool>(
            &index_factory_, MakeRealCoordFactory(), &ns_router_, &unit_router_,
            config_);
    }

    // Stub ListNamespaces to return `ids` as the successful batch results (the
    // catalog NS list StartupLoadAll iterates).
    void StubListNamespaces(const std::vector<std::string>& ids) {
        using ::testing::Invoke;
        ON_CALL(ns_router_, ListNamespaces(_))
            .WillByDefault(Invoke([ids](const cortrix::catalog::ListNamespacesOptions&) {
                cortrix::catalog::BatchResult<std::string> br;
                br.results = ids;
                return Result<cortrix::catalog::BatchResult<std::string>>(br);
            }));
    }

    // Build a pool whose index Open: sleeps `delay`, then fails for any unit_id in
    // `fail_units` (CX_ERR mapped) else returns a FakeIndex. Used by the startup
    // concurrency / timeout / partial-failure tests (UT9-UT12).
    std::unique_ptr<DefaultNamespacePool> MakePoolWithLoad(
        std::chrono::milliseconds delay, std::set<std::string> fail_units = {}) {
        using ::testing::Invoke;
        ON_CALL(ns_router_, GetActiveUnit(_))
            .WillByDefault(Invoke([this](const std::string& ns) {
                return Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
            }));
        ON_CALL(index_factory_, Open(_))
            .WillByDefault(Invoke([delay, fail_units](const std::string& unit_dir) {
                if (delay.count() > 0) std::this_thread::sleep_for(delay);
                // unit_dir ends with "/<ns>-u"; check if this unit must fail.
                for (const auto& fu : fail_units) {
                    if (unit_dir.size() >= fu.size() &&
                        unit_dir.compare(unit_dir.size() - fu.size(), fu.size(), fu) == 0) {
                        return Result<std::unique_ptr<IIndex>>(
                            Status(StatusCode::kUnavailable, "forced load failure"));
                    }
                }
                return Result<std::unique_ptr<IIndex>>(std::make_unique<FakeIndex>(0));
            }));
        return std::make_unique<DefaultNamespacePool>(
            &index_factory_, MakeRealCoordFactory(), &ns_router_, &unit_router_,
            config_);
    }

    std::filesystem::path tmp_root_;
    F05Config config_;
    ::testing::NiceMock<MockIndexFactory> index_factory_;
    ::testing::NiceMock<MockNSRouter> ns_router_;
    ::testing::NiceMock<MockUnitRouter> unit_router_;
    FakeVectorStore vec_store_;
    FakeMetadataStore meta_store_;
    FakeBlobStore blob_store_;
};

// ── UT3 — F05Config defaults ─────────────────────────────────────────────────

TEST(F05ConfigTest, Defaults) {
    F05Config c;
    EXPECT_EQ(c.max_namespaces_per_instance, 64u);
    EXPECT_EQ(c.memory_budget_bytes, 0u);  // disabled by default
    EXPECT_EQ(c.startup_load_workers, 8);
    EXPECT_EQ(c.load_timeout_ms_per_ns, 30000);  // raised from 5000: large healthy NS load guard
    // SQLite PRAGMA defaults (§4.1 / §7.1).
    EXPECT_EQ(c.pragmas.journal_mode, "WAL");
    EXPECT_EQ(c.pragmas.synchronous, "NORMAL");
    EXPECT_EQ(c.pragmas.cache_size_kb, -2000);
    EXPECT_EQ(c.pragmas.mmap_size_bytes, 64 * 1024 * 1024);
    EXPECT_EQ(c.pragmas.temp_store, "MEMORY");
    EXPECT_EQ(c.pragmas.busy_timeout_ms, 5000);
}

// ── UT1 — NamespaceResourceBundle::primary() returns the first Unit ───────────

TEST(NamespaceResourceBundleTest, PrimaryReturnsFirstUnit) {
    NamespaceResourceBundle b;
    b.namespace_id = "ns-1";
    UnitResourceBundle u;
    u.unit_id = "ns-1-u";
    b.unit_bundles.push_back(std::move(u));
    EXPECT_EQ(b.primary().unit_id, "ns-1-u");
    EXPECT_EQ(static_cast<const NamespaceResourceBundle&>(b).primary().unit_id, "ns-1-u");
}

// ── UT2 — destructor releases index / WriteCoordinator / SqliteConn ──────────

TEST(NamespaceResourceBundleTest, DestructorReleasesResources) {
    // A bundle with all three real resources; leaving scope must not leak / crash
    // (run under ASan in CI). We also assert the memory estimate aggregates the
    // F05-2 (index footprint) + F05-3 (pwl size) hooks.
    auto tmp = std::filesystem::temp_directory_path() / "f05_bundle_dtor";
    std::filesystem::create_directories(tmp);
    FakeVectorStore vs;
    FakeMetadataStore ms;
    FakeBlobStore bs;
    {
        NamespaceResourceBundle b;
        b.namespace_id = "ns-d";
        UnitResourceBundle u;
        u.unit_id = "ns-d-u";
        u.index = std::make_unique<FakeIndex>(4096);
        u.pwl = std::make_unique<WriteCoordinator>(tmp.string(),
                                                   WriteCoordinatorConfig{}, &vs, &ms, &bs);
        ASSERT_TRUE(u.pwl->Init().ok());
        u.store_db = std::make_unique<SqliteConn>();
        ASSERT_TRUE(u.store_db->Open((tmp / "store.db").string()).ok());
        b.unit_bundles.push_back(std::move(u));

        // index footprint (4096) + pwl size (>=0) accounted; store.db not double
        // counted (its cache is PRAGMA-bounded, §7.1).
        EXPECT_GE(b.TotalMemoryEstimateBytes(), 4096u);
    }  // <- all resources released here
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ── UT13 — Acquire returns the bundle for a resident NS (O(1) hit) ───────────

TEST_F(NamespacePoolTest, AcquireResidentNamespace) {
    MakeUnitDir("ns-a-u");
    auto pool = MakePool();
    ASSERT_TRUE(pool->AdmitCreate("ns-a", 100).ok());

    auto acq = pool->Acquire("ns-a");
    ASSERT_TRUE(acq.ok());
    ASSERT_NE(acq.value(), nullptr);
    EXPECT_EQ(acq.value()->namespace_id, "ns-a");
    ASSERT_EQ(acq.value()->unit_bundles.size(), 1u);  // Phase 1: 1 NS = 1 Unit
    EXPECT_EQ(acq.value()->primary().unit_id, "ns-a-u");
}

// ── UT14 — Acquire on an unknown NS returns CX_ERR_NS_NOT_FOUND ──────────────

TEST_F(NamespacePoolTest, AcquireUnknownNamespaceNotFound) {
    auto pool = MakePool();
    auto acq = pool->Acquire("ghost");
    ASSERT_FALSE(acq.ok());
    EXPECT_EQ(acq.status().code(), StatusCode::kNotFound);
    // The CX_ERR_NS_NOT_FOUND token is embedded for boundary re-inflation.
    EXPECT_NE(acq.status().message().find("CX_ERR_NS_NOT_FOUND"), std::string::npos);
}

// ── UT4 — the (max+1)-th NS create is rejected with NS_QUOTA_EXCEEDED ────────

TEST_F(NamespacePoolTest, AdmitRejectsBeyondMaxNamespaces) {
    config_.max_namespaces_per_instance = 3;  // small ceiling for the test
    auto pool = MakePool();
    for (int i = 0; i < 3; ++i) {
        std::string ns = "ns-" + std::to_string(i);
        MakeUnitDir(ns + "-u");
        ASSERT_TRUE(pool->AdmitCreate(ns, 100).ok()) << ns;
    }
    MakeUnitDir("ns-overflow-u");
    Status s = pool->AdmitCreate("ns-overflow", 100);
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_NS_QUOTA_EXCEEDED"), std::string::npos);
    // Quota is permanent (non-retryable) → coarse kPermissionDenied.
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    // The pool stayed at the ceiling; the rejected NS is not resident.
    EXPECT_EQ(pool->GetPoolStats().pool_size_current, 3u);
    EXPECT_FALSE(pool->Acquire("ns-overflow").ok());
}

// ── UT5 — memory_budget enabled + over budget → RESOURCE_BUDGET_EXCEEDED ─────

TEST_F(NamespacePoolTest, AdmitRejectsOverMemoryBudget) {
    config_.memory_budget_bytes = 10000;  // enable the budget gate
    auto pool = MakePool(/*index_footprint=*/6000);  // each NS ~6000 bytes resident
    MakeUnitDir("ns-0-u");
    ASSERT_TRUE(pool->AdmitCreate("ns-0", 6000).ok());  // used ~6000 <= 10000

    // Second NS estimate 6000 → used(≈6000) + 6000 = 12000 > 10000 → reject.
    MakeUnitDir("ns-1-u");
    Status s = pool->AdmitCreate("ns-1", 6000);
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_NS_RESOURCE_BUDGET_EXCEEDED"), std::string::npos);
    EXPECT_EQ(pool->GetPoolStats().pool_size_current, 1u);
}

// ── UT6 — memory_budget == 0 disables the memory gate (only count is checked) ─

TEST_F(NamespacePoolTest, MemoryBudgetZeroDisablesMemoryGate) {
    config_.memory_budget_bytes = 0;  // disabled (the default)
    auto pool = MakePool(/*index_footprint=*/1u << 30);  // 1 GiB "resident" each
    MakeUnitDir("ns-0-u");
    MakeUnitDir("ns-1-u");
    // Both admit despite a huge footprint, because the budget gate is off.
    ASSERT_TRUE(pool->AdmitCreate("ns-0", 1u << 30).ok());
    ASSERT_TRUE(pool->AdmitCreate("ns-1", 1u << 30).ok());
    EXPECT_EQ(pool->GetPoolStats().pool_size_current, 2u);
    EXPECT_EQ(pool->GetPoolStats().memory_budget_limit_bytes, 0u);
}

// ── UT7 — F12 rollback: EvictForDelete undoes a pool add (consistency) ───────

TEST_F(NamespacePoolTest, EvictForDeleteUndoesPoolAdd) {
    MakeUnitDir("ns-r-u");
    auto pool = MakePool();
    ASSERT_TRUE(pool->AdmitCreate("ns-r", 100).ok());
    EXPECT_EQ(pool->GetPoolStats().pool_size_current, 1u);

    // Simulates F12 catalog INSERT failure → reverse-undo (§5.2).
    ASSERT_TRUE(pool->EvictForDelete("ns-r").ok());
    EXPECT_EQ(pool->GetPoolStats().pool_size_current, 0u);
    EXPECT_FALSE(pool->Acquire("ns-r").ok());
    // Idempotent: evicting an absent NS is still Ok (rollback of a never-added NS).
    EXPECT_TRUE(pool->EvictForDelete("ns-never").ok());
}

// ── UT8 — rejection log keeps the most recent 50 events (ring buffer) ────────

TEST_F(NamespacePoolTest, RejectionLogIsBoundedRingOf50) {
    config_.max_namespaces_per_instance = 0;  // every create is rejected
    auto pool = MakePool();
    for (int i = 0; i < 60; ++i) {
        Status s = pool->AdmitCreate("ns-rej-" + std::to_string(i), 100);
        ASSERT_FALSE(s.ok());
    }
    auto explain = pool->GetExplainState();
    EXPECT_EQ(explain.recent_rejections.size(), 50u);  // capped at 50
    // Oldest 10 dropped: the buffer's front is the 11th rejection (index 10).
    EXPECT_EQ(explain.recent_rejections.front().namespace_id, "ns-rej-10");
    EXPECT_EQ(explain.recent_rejections.back().namespace_id, "ns-rej-59");
    for (const auto& ev : explain.recent_rejections) {
        EXPECT_EQ(ev.reason, "ns_count_exceeded");
    }
    // The A-class counter saw all 60 even though the C-class log keeps 50.
    EXPECT_EQ(pool->GetPoolStats().rejected_creates_total.ns_count_exceeded, 60u);
}

// ── UT18 — PoolStats (A class) + ExplainState (C class) independent surfaces ──

TEST_F(NamespacePoolTest, StatsAndExplainSurfaces) {
    config_.max_namespaces_per_instance = 64;
    config_.memory_budget_bytes = 1u << 30;
    auto pool = MakePool(/*index_footprint=*/2048);
    MakeUnitDir("ns-x-u");
    MakeUnitDir("ns-y-u");
    ASSERT_TRUE(pool->AdmitCreate("ns-x", 2048).ok());
    ASSERT_TRUE(pool->AdmitCreate("ns-y", 2048).ok());

    PoolStats stats = pool->GetPoolStats();
    EXPECT_EQ(stats.pool_size_current, 2u);
    EXPECT_EQ(stats.pool_size_max, 64u);
    EXPECT_EQ(stats.memory_budget_limit_bytes, 1u << 30);
    EXPECT_GE(stats.memory_budget_used_bytes, 4096u);  // 2 * 2048 index footprint
    EXPECT_EQ(stats.rejected_creates_total.ns_count_exceeded, 0u);
    EXPECT_EQ(stats.rejected_creates_total.memory_exceeded, 0u);
    EXPECT_EQ(stats.startup_load_failures, 0u);

    ExplainState ex = pool->GetExplainState();
    EXPECT_EQ(ex.active_namespaces.size(), 2u);
    EXPECT_GE(ex.memory_estimate_bytes, 4096u);
    EXPECT_TRUE(ex.startup_failed_namespaces.empty());
    EXPECT_TRUE(ex.recent_rejections.empty());
}

// ── load-failure path: index Open failure → AdmitCreate CX_ERR_NS_LOAD_FAILED ─

TEST_F(NamespacePoolTest, AdmitLoadFailureSurfacesLoadFailed) {
    using ::testing::Invoke;
    ON_CALL(ns_router_, GetActiveUnit(_))
        .WillByDefault(Invoke([this](const std::string& ns) {
            return Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
        }));
    // Index factory fails to Open → load fails.
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string&) {
            return Result<std::unique_ptr<IIndex>>(
                Status(StatusCode::kUnavailable, "index file corruption"));
        }));
    DefaultNamespacePool pool(&index_factory_, MakeRealCoordFactory(), &ns_router_,
                              &unit_router_, config_);
    Status s = pool.AdmitCreate("ns-bad", 100);
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_NS_LOAD_FAILED"), std::string::npos);
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);  // transient → retryable
    EXPECT_EQ(pool.GetPoolStats().pool_size_current, 0u);  // nothing added
}

// ── PoolError registry: the 4-code §8.1 identity set ─────────────────────────

TEST(PoolErrorTest, FourCodesWellFormedAndMapped) {
    EXPECT_EQ(kPoolErrorCodeCount, 4);
    struct Row {
        PoolErrorCode code;
        const char* cx;
        agent_friendly::ErrorCategory cat;
        bool retryable;
        StatusCode status;
    };
    const Row rows[] = {
        {PoolErrorCode::kNsQuotaExceeded, "CX_ERR_NS_QUOTA_EXCEEDED",
         agent_friendly::ErrorCategory::kQuota, false, StatusCode::kPermissionDenied},
        {PoolErrorCode::kNsResourceBudgetExceeded, "CX_ERR_NS_RESOURCE_BUDGET_EXCEEDED",
         agent_friendly::ErrorCategory::kQuota, false, StatusCode::kPermissionDenied},
        {PoolErrorCode::kNsLoadFailed, "CX_ERR_NS_LOAD_FAILED",
         agent_friendly::ErrorCategory::kTransient, true, StatusCode::kUnavailable},
        {PoolErrorCode::kNsNotFound, "CX_ERR_NS_NOT_FOUND",
         agent_friendly::ErrorCategory::kPermanent, false, StatusCode::kNotFound},
    };
    for (const Row& r : rows) {
        const PoolErrorInfo& info = GetPoolErrorInfo(r.code);
        EXPECT_STREQ(info.cx_code, r.cx);
        EXPECT_EQ(info.category, r.cat);
        EXPECT_EQ(info.retryable, r.retryable);
        EXPECT_EQ(info.retryable, info.retry_after_ms.has_value());
        EXPECT_EQ(PoolErrorToStatusCode(r.code), r.status);
    }
}

// MakePoolError builds the GEN-Agent body (§8.3), and HasRequiredStructuredData
// enforces the §8.1 required-key set.
TEST(PoolErrorTest, MakePoolErrorBodyAndRequiredKeys) {
    nlohmann::json sd = {{"current_count", 64}, {"limit", 64}, {"namespace_id", "ns-z"}};
    EXPECT_TRUE(HasRequiredStructuredData(PoolErrorCode::kNsQuotaExceeded, sd));
    EXPECT_FALSE(HasRequiredStructuredData(PoolErrorCode::kNsQuotaExceeded,
                                           {{"limit", 64}}));  // missing keys

    auto err = MakePoolError(PoolErrorCode::kNsLoadFailed,
                             {{"namespace_id", "ns-c"},
                              {"load_step", "open_index"},
                              {"error_detail", "magic mismatch"}},
                             "load failed");
    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_NS_LOAD_FAILED");
    EXPECT_EQ(body["category"], "transient");
    EXPECT_EQ(body["retryable"], true);
    EXPECT_EQ(body["retry_after_ms"], 1000);
    EXPECT_EQ(body["structured_data"]["load_step"], "open_index");
}

// ════════════════════════ S2: startup / reload / PRAGMA ════════════════════

// ── UT9 — 8 workers load 64 NS concurrently (mock 100ms/NS → total < 1s) ─────

TEST_F(NamespacePoolTest, StartupLoadEightWorkersConcurrency) {
    config_.startup_load_workers = 8;
    config_.load_timeout_ms_per_ns = 5000;  // well above the 100ms mock load
    std::vector<std::string> ids;
    for (int i = 0; i < 64; ++i) {
        std::string ns = "ns-" + std::to_string(i);
        ids.push_back(ns);
        MakeUnitDir(ns + "-u");
    }
    StubListNamespaces(ids);
    auto pool = MakePoolWithLoad(std::chrono::milliseconds(100));

    const auto t0 = std::chrono::steady_clock::now();
    auto rep = pool->StartupLoadAll();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    ASSERT_TRUE(rep.ok());
    EXPECT_EQ(rep.value().total_namespaces, 64u);
    EXPECT_EQ(rep.value().loaded_successfully, 64u);
    EXPECT_EQ(rep.value().failed, 0u);
    EXPECT_EQ(pool->GetPoolStats().pool_size_current, 64u);
    // 64 NS / 8 workers = 8 serial batches * 100ms ≈ 800ms. Sequential would be
    // ~6.4s. Generous ceiling (2s) keeps it non-flaky on a loaded CI box while
    // still proving the load ran concurrently, not serially.
    EXPECT_LT(elapsed, 2000) << "elapsed=" << elapsed << "ms (expected concurrent)";
}

// ── UT10 — a single NS whose load exceeds the timeout is counted as failure ──

TEST_F(NamespacePoolTest, StartupSingleNsTimeoutCountsAsFailure) {
    config_.startup_load_workers = 4;
    config_.load_timeout_ms_per_ns = 150;  // tight timeout
    std::vector<std::string> ids = {"fast-0", "slow-1", "fast-2"};
    for (auto& ns : ids) MakeUnitDir(ns + "-u");
    StubListNamespaces(ids);
    // "slow-1" sleeps 600ms > 150ms timeout; the others are fast (we give every
    // Open the same delay, so make the timeout the discriminator: use a single
    // delay just above the timeout for ALL, then assert all time out). To isolate
    // ONE slow NS we instead force slow-1 to block long via a dedicated factory.
    using ::testing::Invoke;
    ON_CALL(ns_router_, GetActiveUnit(_))
        .WillByDefault(Invoke([this](const std::string& ns) {
            return Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
        }));
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string& unit_dir) {
            if (unit_dir.find("slow-1") != std::string::npos) {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
            }
            return Result<std::unique_ptr<IIndex>>(std::make_unique<FakeIndex>(0));
        }));
    DefaultNamespacePool pool(&index_factory_, MakeRealCoordFactory(), &ns_router_,
                              &unit_router_, config_);

    auto rep = pool.StartupLoadAll();
    ASSERT_TRUE(rep.ok());
    EXPECT_EQ(rep.value().total_namespaces, 3u);
    EXPECT_EQ(rep.value().loaded_successfully, 2u);
    EXPECT_EQ(rep.value().failed, 1u);
    ASSERT_EQ(rep.value().failed_namespaces.size(), 1u);
    EXPECT_EQ(rep.value().failed_namespaces[0], "slow-1");
    EXPECT_EQ(pool.GetPoolStats().startup_load_failures, 1u);
    // The 2 fast NS are resident; the timed-out one is not (admin can reload it).
    EXPECT_TRUE(pool.Acquire("fast-0").ok());
    EXPECT_FALSE(pool.Acquire("slow-1").ok());
    // It shows up in the C-class explain startup-failed list.
    auto ex = pool.GetExplainState();
    EXPECT_EQ(ex.startup_failed_namespaces.size(), 1u);
    EXPECT_EQ(ex.startup_failed_namespaces[0], "slow-1");
}

// ── UT10b — ReloadNamespace recovers a startup-timed-out NS WITHOUT the timeout ──
// Regression for the F05 large-NS bug (2026-06-20): a large-but-healthy NS whose load
// exceeds the per-NS startup timeout was left permanently un-admitted because
// ReloadNamespace reused the SAME timeout. ReloadNamespace now loads without the
// timeout, so a slow load that exceeds the (tight) startup timeout still recovers.
TEST_F(NamespacePoolTest, ReloadRecoversStartupTimedOutNsIgnoringTimeout) {
    config_.startup_load_workers = 4;
    config_.load_timeout_ms_per_ns = 150;  // tight: the 600ms load times out at startup
    std::vector<std::string> ids = {"big-slow-1"};
    MakeUnitDir("big-slow-1-u");
    StubListNamespaces(ids);
    using ::testing::Invoke;
    ON_CALL(ns_router_, GetActiveUnit(_))
        .WillByDefault(Invoke([this](const std::string& ns) {
            return Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
        }));
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([](const std::string& /*unit_dir*/) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));  // > 150ms timeout
            return Result<std::unique_ptr<IIndex>>(std::make_unique<FakeIndex>(0));
        }));
    DefaultNamespacePool pool(&index_factory_, MakeRealCoordFactory(), &ns_router_,
                              &unit_router_, config_);

    // Startup: the slow load exceeds the 150ms guard -> NS not admitted (the bug's setup).
    auto rep = pool.StartupLoadAll();
    ASSERT_TRUE(rep.ok());
    EXPECT_EQ(rep.value().failed, 1u);
    EXPECT_FALSE(pool.Acquire("big-slow-1").ok());

    // The fix: ReloadNamespace loads WITHOUT the per-NS timeout, so the 600ms load that
    // would time out at startup (and under the OLD timeout-guarded reload) now succeeds.
    Status reload = pool.ReloadNamespace("big-slow-1");
    ASSERT_TRUE(reload.ok()) << reload.message();
    EXPECT_TRUE(pool.Acquire("big-slow-1").ok());
    // And it's scrubbed from the startup-failed explain list.
    EXPECT_TRUE(pool.GetExplainState().startup_failed_namespaces.empty());
}

// ── UT11 — a failing NS load does not block the others ───────────────────────

TEST_F(NamespacePoolTest, StartupPartialFailureDoesNotBlockOthers) {
    config_.startup_load_workers = 4;
    config_.load_timeout_ms_per_ns = 5000;
    std::vector<std::string> ids = {"ok-0", "bad-1", "ok-2", "bad-3", "ok-4"};
    for (auto& ns : ids) MakeUnitDir(ns + "-u");
    StubListNamespaces(ids);
    auto pool = MakePoolWithLoad(std::chrono::milliseconds(0),
                                 /*fail_units=*/{"bad-1-u", "bad-3-u"});

    auto rep = pool->StartupLoadAll();
    ASSERT_TRUE(rep.ok());
    EXPECT_EQ(rep.value().total_namespaces, 5u);
    EXPECT_EQ(rep.value().loaded_successfully, 3u);
    EXPECT_EQ(rep.value().failed, 2u);
    EXPECT_TRUE(pool->Acquire("ok-0").ok());
    EXPECT_TRUE(pool->Acquire("ok-2").ok());
    EXPECT_TRUE(pool->Acquire("ok-4").ok());
    EXPECT_FALSE(pool->Acquire("bad-1").ok());
    EXPECT_FALSE(pool->Acquire("bad-3").ok());
}

// ── UT12 — catalog NS count > max → load only the first max, log a warning ───

TEST_F(NamespacePoolTest, StartupTruncatesToMaxNamespaces) {
    config_.max_namespaces_per_instance = 3;
    config_.load_timeout_ms_per_ns = 5000;
    std::vector<std::string> ids = {"a", "b", "c", "d", "e"};  // 5 > max(3)
    for (auto& ns : ids) MakeUnitDir(ns + "-u");
    StubListNamespaces(ids);
    auto pool = MakePoolWithLoad(std::chrono::milliseconds(0));

    auto rep = pool->StartupLoadAll();
    ASSERT_TRUE(rep.ok());
    EXPECT_EQ(rep.value().total_namespaces, 3u);  // truncated
    EXPECT_EQ(rep.value().loaded_successfully, 3u);
    EXPECT_EQ(pool->GetPoolStats().pool_size_current, 3u);
    // First three loaded; the overflow two were never attempted.
    EXPECT_TRUE(pool->Acquire("a").ok());
    EXPECT_TRUE(pool->Acquire("c").ok());
    EXPECT_FALSE(pool->Acquire("d").ok());
    EXPECT_FALSE(pool->Acquire("e").ok());
}

// ── UT15 — ReloadNamespace retries a previously-failed NS (success path) ─────

TEST_F(NamespacePoolTest, ReloadNamespaceSucceedsAndClearsFailedList) {
    config_.load_timeout_ms_per_ns = 5000;
    StubListNamespaces({"flaky"});
    MakeUnitDir("flaky-u");
    // First the index Open fails (startup), then a later Open succeeds (reload).
    using ::testing::Invoke;
    ON_CALL(ns_router_, GetActiveUnit(_))
        .WillByDefault(Invoke([this](const std::string& ns) {
            return Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
        }));
    int open_calls = 0;
    ON_CALL(index_factory_, Open(_))
        .WillByDefault(Invoke([&open_calls](const std::string&) {
            if (++open_calls == 1) {
                return Result<std::unique_ptr<IIndex>>(
                    Status(StatusCode::kUnavailable, "transient at startup"));
            }
            return Result<std::unique_ptr<IIndex>>(std::make_unique<FakeIndex>(0));
        }));
    DefaultNamespacePool pool(&index_factory_, MakeRealCoordFactory(), &ns_router_,
                              &unit_router_, config_);

    auto rep = pool.StartupLoadAll();
    ASSERT_TRUE(rep.ok());
    EXPECT_EQ(rep.value().failed, 1u);
    EXPECT_FALSE(pool.Acquire("flaky").ok());
    EXPECT_EQ(pool.GetExplainState().startup_failed_namespaces.size(), 1u);

    // Admin retry now succeeds.
    Status reload = pool.ReloadNamespace("flaky");
    ASSERT_TRUE(reload.ok());
    EXPECT_TRUE(pool.Acquire("flaky").ok());
    EXPECT_EQ(pool.GetPoolStats().pool_size_current, 1u);
    // The NS is cleared from the startup-failed list after a successful reload.
    EXPECT_TRUE(pool.GetExplainState().startup_failed_namespaces.empty());
}

// ── UT16 — ReloadNamespace still failing returns CX_ERR_NS_LOAD_FAILED ───────

TEST_F(NamespacePoolTest, ReloadNamespaceStillFailingReturnsLoadFailed) {
    config_.load_timeout_ms_per_ns = 5000;
    MakeUnitDir("corrupt-u");
    auto pool = MakePoolWithLoad(std::chrono::milliseconds(0),
                                 /*fail_units=*/{"corrupt-u"});
    Status s = pool->ReloadNamespace("corrupt");
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_NS_LOAD_FAILED"), std::string::npos);
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_FALSE(pool->Acquire("corrupt").ok());
}

// ── UT17 — ApplyPragmas sets all 6 PRAGMAs (verified via read-back) ──────────

TEST_F(NamespacePoolTest, ApplyPragmasSetsAllSixVerifiedByReadback) {
    auto dir = tmp_root_ / "pragma-unit";
    std::filesystem::create_directories(dir);
    SqliteConn conn;
    ASSERT_TRUE(conn.Open((dir / "store.db").string()).ok());

    SqlitePragmas p;  // defaults (§4.1)
    ASSERT_TRUE(conn.ApplyPragmas(p).ok());

    // Read each PRAGMA back and confirm it took. SQLite normalizes some values
    // (journal_mode → lowercase "wal"; synchronous → numeric "1" for NORMAL;
    // temp_store → "2" for MEMORY), so we assert on the normalized form.
    auto jm = conn.ReadPragma("journal_mode");
    ASSERT_TRUE(jm.ok());
    EXPECT_EQ(jm.value(), "wal");
    auto sync = conn.ReadPragma("synchronous");
    ASSERT_TRUE(sync.ok());
    EXPECT_EQ(sync.value(), "1");  // NORMAL
    auto temp = conn.ReadPragma("temp_store");
    ASSERT_TRUE(temp.ok());
    EXPECT_EQ(temp.value(), "2");  // MEMORY
    auto cache = conn.ReadPragma("cache_size");
    ASSERT_TRUE(cache.ok());
    EXPECT_EQ(cache.value(), "-2000");
    auto mmap = conn.ReadPragma("mmap_size");
    ASSERT_TRUE(mmap.ok());
    EXPECT_EQ(mmap.value(), std::to_string(64 * 1024 * 1024));
    auto busy = conn.ReadPragma("busy_timeout");
    ASSERT_TRUE(busy.ok());
    EXPECT_EQ(busy.value(), "5000");
}

// ── Startup with an empty catalog is a clean no-op ───────────────────────────

TEST_F(NamespacePoolTest, StartupEmptyCatalogLoadsNothing) {
    StubListNamespaces({});
    auto pool = MakePoolWithLoad(std::chrono::milliseconds(0));
    auto rep = pool->StartupLoadAll();
    ASSERT_TRUE(rep.ok());
    EXPECT_EQ(rep.value().total_namespaces, 0u);
    EXPECT_EQ(rep.value().loaded_successfully, 0u);
    EXPECT_EQ(pool->GetPoolStats().pool_size_current, 0u);
}

// ── D3.5 wire④: NamespaceFacade smoke tests (reuse the pool fixture) ──────────
// The façade is the thin layer over Pool.Acquire (§4). These prove the five
// windows wire up and, critically, that the D-I1 external-conn CortrixStore works
// against the bundle's own store.db connection (no second open of the file).

TEST_F(NamespacePoolTest, FacadeAcquireExposesFiveWindows) {
    MakeUnitDir("ns-fac-u");
    auto pool = MakePool();
    ASSERT_TRUE(pool->AdmitCreate("ns-fac", 100).ok());

    NamespaceFacade facade(*pool, "ns-fac");
    ASSERT_TRUE(facade.Acquire().ok());
    EXPECT_TRUE(facade.acquired());

    // vec_index() + write_coordinator() forward the bundle's heavy resources;
    // taking the references proves the wiring (FakeIndex + the real WriteCoordinator).
    (void)facade.vec_index();
    (void)facade.write_coordinator();

    // store() = D-I1 external-conn CortrixStore over the bundle's store.db. Prove
    // the external-conn mode works end to end: create + read back a doc on the
    // pool's own connection (and that doc_create mints a ULID, D-I6).
    cortrix::CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "/facade/a.txt";
    doc.content_hash = "h1";
    ASSERT_EQ(facade.store().doc_create(doc), 0);
    EXPECT_FALSE(doc.doc_id.empty());
    cortrix::CortrixDoc got;
    ASSERT_EQ(facade.store().doc_get(doc.doc_id, got), 0);
    EXPECT_EQ(got.doc_id, doc.doc_id);
    EXPECT_EQ(got.source_path, "/facade/a.txt");

    // memory() = self-held independent memory.db under <unit_dir> (D-I4).
    cortrix::MemorySession sess;
    sess.namespace_name = "ns-fac";
    Status ms = facade.memory().SessionCreate(sess);
    EXPECT_TRUE(ms.ok()) << ms.message();
    EXPECT_FALSE(sess.session_id.empty());

    // blob() = self-held blob store under <unit_dir>/blob (D-I4). Round-trip.
    const char payload[] = "hello-blob";
    ASSERT_EQ(facade.blob().store("ns-fac", doc.doc_id, payload, sizeof(payload)), 0);
    std::vector<uint8_t> out;
    ASSERT_EQ(facade.blob().load("ns-fac", doc.doc_id, out), 0);
    EXPECT_EQ(out.size(), sizeof(payload));
}

TEST_F(NamespacePoolTest, FacadeReleaseOnDestroyThenReacquire) {
    MakeUnitDir("ns-rel-u");
    auto pool = MakePool();
    ASSERT_TRUE(pool->AdmitCreate("ns-rel", 100).ok());
    {
        NamespaceFacade f(*pool, "ns-rel");
        ASSERT_TRUE(f.Acquire().ok());
    }  // ~NamespaceFacade → Release (Phase 1 no-op) must not crash
    NamespaceFacade f2(*pool, "ns-rel");
    EXPECT_TRUE(f2.Acquire().ok());
}

TEST_F(NamespacePoolTest, FacadeAcquireUnknownNamespaceFails) {
    auto pool = MakePool();
    NamespaceFacade f(*pool, "ghost");
    EXPECT_FALSE(f.Acquire().ok());
    EXPECT_FALSE(f.acquired());
}

// ── facade Acquire failure sub-paths (store.db / memory.db open) ──────────────
// The NS is admitted (pool load creates store.db), then the per-facade store.db /
// memory.db path is swapped for a DIRECTORY so the façade's own open of that file
// fails — exercising the namespace_facade.cpp store-open / memory-init branches
// that the resident-NS happy path never hits.

TEST_F(NamespacePoolTest, FacadeStoreOpenFailureSurfacesInternal) {
    MakeUnitDir("ns-sf-u");
    auto pool = MakePool();
    ASSERT_TRUE(pool->AdmitCreate("ns-sf", 100).ok());

    // Replace the per-facade store.db file with a directory of the same name; the
    // façade's CortrixStoreSqlite::Open() (sqlite3_open on a dir) then fails.
    const auto store_db = tmp_root_ / "ns-sf-u" / "store.db";
    std::error_code ec;
    std::filesystem::remove(store_db, ec);
    std::filesystem::create_directory(store_db, ec);

    NamespaceFacade facade(*pool, "ns-sf");
    Status s = facade.Acquire();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("store Open() failed"), std::string::npos) << s.message();
    // acquired_ is set once the POOL acquire succeeds (before the window
    // build) so the dtor still Releases the bundle; a failed window build
    // leaves the facade acquired-but-unusable by design.
    EXPECT_TRUE(facade.acquired());
}

TEST_F(NamespacePoolTest, FacadeMemoryInitFailureSurfacesInternal) {
    MakeUnitDir("ns-mf-u");
    auto pool = MakePool();
    ASSERT_TRUE(pool->AdmitCreate("ns-mf", 100).ok());

    // Leave store.db intact (store Open must succeed first) but make memory.db a
    // directory so MemoryStore::Init() fails — the second façade failure branch.
    const auto mem_db = tmp_root_ / "ns-mf-u" / "memory.db";
    std::error_code ec;
    std::filesystem::remove(mem_db, ec);
    std::filesystem::create_directory(mem_db, ec);

    NamespaceFacade facade(*pool, "ns-mf");
    Status s = facade.Acquire();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("memory Init() failed"), std::string::npos) << s.message();
    // acquired_ is set once the POOL acquire succeeds (before the window
    // build) so the dtor still Releases the bundle; a failed window build
    // leaves the facade acquired-but-unusable by design.
    EXPECT_TRUE(facade.acquired());
}

}  // namespace
}  // namespace cortrix::resource
