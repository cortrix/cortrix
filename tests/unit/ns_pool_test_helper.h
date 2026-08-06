#pragma once
// integration-wiring test helper — shared NamespacePool test doubles + harness.
//
// The wire⑤c migration moved every consumer off the MVP CortrixNamespaceManager
// onto the namespace pool resource::INamespacePool + per-request resource::NamespaceFacade.
// Their unit tests therefore need a real DefaultNamespacePool (mocked catalog routers
// + a capturing FakeIndex + a real WriteCoordinator over a temp dir), exactly
// as the spc pilot's fixture stands one up (tests/unit/test_spc_pipeline.cpp).
//
// This header extracts that proven setup so the route/consumer tests share ONE
// copy. Use NsPoolHarness by composition in a fixture:
//
//   class MyTest : public ::testing::Test {
//    protected:
//     void SetUp() override {
//       harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
//           std::filesystem::temp_directory_path() / ("mytest_" + ...));
//       ASSERT_TRUE(harness_->Admit("test-ns").ok());
//       // routes/consumers take harness_->pool();  vector asserts via harness_->fake_index()
//     }
//     std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
//   };
//
// Tests that only need a vector index (e.g. VectorSearcher) can use FakeIndex
// directly without a pool.

#include <gmock/gmock.h>

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
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/iindex.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/write_coordinator.h"

namespace cortrix::test {

// IIndex + IVectorStore stand-in. CAPTURES the ids handed to AddPoint/AddPoints
// (so a test can assert a vector write went through facade.vec_index()) and can
// be told to fail (the C2 rollback path). PHnsw implements both bases, so the
// pool's cross-cast to IVectorStore (integration C1) succeeds against this fake too.
class FakeIndex : public cortrix::store::IIndex, public cortrix::store::IVectorStore {
public:
    explicit FakeIndex(std::size_t footprint = 0) : footprint_(footprint) {}

    // —— shared by IIndex + IVectorStore (identical signatures) ——
    cortrix::Status AddPoint(const float*, uint64_t id,
                             const cortrix::observability::TraceContext*) override {
        if (add_should_fail_) return cortrix::Status(cortrix::StatusCode::kUnavailable, "forced add failure");
        added_ids_.push_back(id);
        return cortrix::Status::Ok();
    }
    cortrix::Status MarkDelete(uint64_t id, const cortrix::observability::TraceContext*) override {
        deleted_ids_.push_back(id);
        return cortrix::Status::Ok();
    }
    // —— IIndex-only ——
    cortrix::Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>& pts,
                              const cortrix::observability::TraceContext*) override {
        if (add_should_fail_) return cortrix::Status(cortrix::StatusCode::kUnavailable, "forced add failure");
        for (const auto& p : pts) added_ids_.push_back(p.second);
        return cortrix::Status::Ok();
    }
    std::vector<std::pair<uint64_t, float>> Search(const float*, int, int,
                                                   const cortrix::observability::TraceContext*) override {
        return search_result_;
    }
    bool Exists(uint64_t) override { return false; }
    cortrix::Status Snapshot() override { return cortrix::Status::Ok(); }
    cortrix::Status Recover() override { return cortrix::Status::Ok(); }
    cortrix::Status Shutdown() override { return cortrix::Status::Ok(); }
    cortrix::store::IndexStats GetStats() override { return cortrix::store::IndexStats{}; }
    std::size_t GetMemoryFootprintBytes() const override { return footprint_; }
    // —— IVectorStore-only ——
    std::vector<std::pair<uint64_t, float>> Search(const float*, int,
                                                   const cortrix::observability::TraceContext*) override {
        return search_result_;
    }
    bool Exists(uint64_t, const cortrix::observability::TraceContext*) override { return false; }
    cortrix::Status Snapshot(const cortrix::observability::TraceContext*) override { return cortrix::Status::Ok(); }
    cortrix::Status Recover(const cortrix::observability::TraceContext*) override { return cortrix::Status::Ok(); }

    void set_add_should_fail(bool v) { add_should_fail_ = v; }
    void set_search_result(std::vector<std::pair<uint64_t, float>> r) { search_result_ = std::move(r); }
    const std::vector<uint64_t>& added_ids() const { return added_ids_; }
    const std::vector<uint64_t>& deleted_ids() const { return deleted_ids_; }

private:
    std::size_t footprint_;
    std::vector<uint64_t> added_ids_;
    std::vector<uint64_t> deleted_ids_;
    std::vector<std::pair<uint64_t, float>> search_result_;
    bool add_should_fail_ = false;
};

class MockIndexFactory : public cortrix::store::IIndexFactory {
public:
    MOCK_METHOD((cortrix::Result<std::unique_ptr<cortrix::store::IIndex>>), Create,
                (const std::string&, const cortrix::store::IndexConfig&), (override));
    MOCK_METHOD((cortrix::Result<std::unique_ptr<cortrix::store::IIndex>>), Open,
                (const std::string&), (override));
};

class MockNSRouter : public cortrix::catalog::INSRouter {
public:
    MOCK_METHOD((cortrix::Result<std::vector<cortrix::catalog::UnitDescriptor>>), LookupUnits,
                (const std::string&), (const, override));
    MOCK_METHOD((cortrix::Result<cortrix::catalog::UnitDescriptor>), GetActiveUnit,
                (const std::string&), (const, override));
    MOCK_METHOD((cortrix::Result<cortrix::catalog::NSMetadata>), GetNamespace,
                (const std::string&), (const, override));
    MOCK_METHOD((cortrix::Result<cortrix::catalog::BatchResult<std::string>>), ListNamespaces,
                (const cortrix::catalog::ListNamespacesOptions&), (const, override));
    MOCK_METHOD(cortrix::Status, CreateNamespace, (const cortrix::catalog::NSMetadata&), (override));
    MOCK_METHOD(cortrix::Status, DeleteNamespace, (const std::string&), (override));
};

class MockUnitRouter : public cortrix::catalog::IUnitRouter {
public:
    MOCK_METHOD((cortrix::Result<cortrix::catalog::UnitDescriptor>), GetUnit,
                (const std::string&), (const, override));
    MOCK_METHOD((cortrix::Result<cortrix::catalog::UnitState>), GetUnitState,
                (const std::string&), (const, override));
    MOCK_METHOD(cortrix::Status, UpdateUnitState,
                (const std::string&, cortrix::catalog::UnitState), (override));
    MOCK_METHOD(cortrix::Status, UpdateUnitStats,
                (const std::string&, const cortrix::catalog::UnitStats&), (override));
    MOCK_METHOD((cortrix::Result<bool>), ShouldSeal, (const std::string&), (const, override));
};

// Bundles the mocked catalog routers + a real-WriteCoordinator DefaultNamespacePool
// rooted at a temp dir. Admit(ns) creates the per-Unit dir and admits the NS so
// Acquire/façade hit. Use by composition. The most recently Open()ed FakeIndex is
// captured in fake_index() for vector-write assertions.
class NsPoolHarness {
public:
    explicit NsPoolHarness(std::filesystem::path data_root)
        : data_root_(std::move(data_root)) {
        std::filesystem::create_directories(data_root_);
        config_.data_root = data_root_.string();
        pool_ = MakePool();
    }

    ~NsPoolHarness() {
        pool_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_root_, ec);
    }

    NsPoolHarness(const NsPoolHarness&) = delete;
    NsPoolHarness& operator=(const NsPoolHarness&) = delete;

    cortrix::resource::DefaultNamespacePool& pool() { return *pool_; }
    cortrix::resource::INamespacePool& ipool() { return *pool_; }
    FakeIndex* fake_index() const { return fake_index_; }
    const std::filesystem::path& data_root() const { return data_root_; }

    // Create the NS's per-Unit dir (store.db / pending.wal home) + admit it.
    cortrix::Status Admit(const std::string& ns, size_t est_bytes = 100) {
        std::filesystem::create_directories(data_root_ / (ns + "-u"));
        return pool_->AdmitCreate(ns, est_bytes);
    }

private:
    cortrix::catalog::UnitDescriptor Unit(const std::string& unit_id) {
        cortrix::catalog::UnitDescriptor u;
        u.unit_id = unit_id;
        u.tenant_id = "t1";
        return u;
    }

    cortrix::resource::WriteCoordinatorFactory MakeRealCoordFactory() {
        return [](const std::string& data_dir, const cortrix::store::WriteCoordinatorConfig& cfg,
                  cortrix::store::IVectorStore* vs, cortrix::store::IMetadataStore* ms,
                  cortrix::store::IBlobStore* bs)
                   -> cortrix::Result<std::unique_ptr<cortrix::store::WriteCoordinator>> {
            auto coord = std::make_unique<cortrix::store::WriteCoordinator>(data_dir, cfg, vs, ms, bs);
            cortrix::Status init = coord->Init();
            if (!init.ok()) return init;
            return cortrix::Result<std::unique_ptr<cortrix::store::WriteCoordinator>>(std::move(coord));
        };
    }

    std::unique_ptr<cortrix::resource::DefaultNamespacePool> MakePool() {
        using ::testing::_;
        using ::testing::Invoke;
        ON_CALL(ns_router_, GetActiveUnit(_))
            .WillByDefault(Invoke([this](const std::string& ns) {
                return cortrix::Result<cortrix::catalog::UnitDescriptor>(Unit(ns + "-u"));
            }));
        ON_CALL(index_factory_, Open(_))
            .WillByDefault(Invoke([this](const std::string&) {
                auto idx = std::make_unique<FakeIndex>(0);
                fake_index_ = idx.get();
                return cortrix::Result<std::unique_ptr<cortrix::store::IIndex>>(std::move(idx));
            }));
        return std::make_unique<cortrix::resource::DefaultNamespacePool>(
            &index_factory_, MakeRealCoordFactory(), &ns_router_, &unit_router_, config_);
    }

    std::filesystem::path data_root_;
    ::testing::NiceMock<MockNSRouter> ns_router_;
    ::testing::NiceMock<MockUnitRouter> unit_router_;
    ::testing::NiceMock<MockIndexFactory> index_factory_;
    FakeIndex* fake_index_ = nullptr;
    cortrix::resource::NamespacePoolConfig config_;
    std::unique_ptr<cortrix::resource::DefaultNamespacePool> pool_;
};

}  // namespace cortrix::test
