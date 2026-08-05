#pragma once
// Reusable deterministic fakes for the doc-discovery surface
// (src/doc_summary/discover_handler.cpp). Two layers:
//
//   1. Standalone fakes for the pure RecallDocSummaryHnsw(IIndex&, CortrixStore&,
//      OnnxEmbedder&, ...) core:
//        - FakeDiscoveryIndex : store::IIndex  -> Search returns a settable list of
//          (block_id, distance) pairs (or throws on demand, the S8.2 degrade path).
//        - FakeDocStore       : CortrixStore   -> in-memory block_id -> CortrixBlock
//          map seeded with block_type=17 doc_summary rows.
//
//   2. DiscoveryPoolHarness for the ExecuteDocDiscovery / HandleDocumentsDiscover
//      endpoints, which internally construct a real resource::NamespaceFacade over a
//      real DefaultNamespacePool (the facade opens its OWN on-disk store.db, so the
//      pool path needs a real per-Unit dir). Modeled on tests/unit/ns_pool_test_helper.h
//      but lets a test inject the per-Unit IIndex factory, so the HNSW-Search-throws
//      degrade path is reachable. The harness exposes a scoped facade to seed the
//      real store.db with block_type=17 rows + create the doc_fts5_index table.
//
// Deterministic: no network / no real model / no real clock.

#include <gmock/gmock.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/catalog/i_unit_router.h"
#include "cortrix/common/data_types.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/resource/namespace_pool_config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/iindex.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/write_coordinator.h"

namespace cortrix::doc_summary::test {

// ---------------------------------------------------------------------------
// Layer 1a: configurable IIndex whose Search returns fixed (block_id, distance)
// pairs, or throws std::runtime_error (the query-time HNSW fault degrade path).
// ---------------------------------------------------------------------------
class FakeDiscoveryIndex : public cortrix::store::IIndex,
                           public cortrix::store::IVectorStore {
public:
    void set_search_result(std::vector<std::pair<uint64_t, float>> r) {
        search_result_ = std::move(r);
    }
    void set_search_throws(bool v) { search_throws_ = v; }

    // --- IIndex search ---
    std::vector<std::pair<uint64_t, float>> Search(
        const float*, int, int,
        const cortrix::observability::TraceContext*) override {
        if (search_throws_) throw std::runtime_error("forced hnsw search fault");
        return search_result_;
    }
    // --- IVectorStore search (different arity) ---
    std::vector<std::pair<uint64_t, float>> Search(
        const float*, int,
        const cortrix::observability::TraceContext*) override {
        if (search_throws_) throw std::runtime_error("forced hnsw search fault");
        return search_result_;
    }

    // --- writes (unused by the recall read path) ---
    cortrix::Status AddPoint(const float*, uint64_t,
                             const cortrix::observability::TraceContext*) override {
        return cortrix::Status::Ok();
    }
    cortrix::Status AddPoints(
        const std::vector<std::pair<const float*, uint64_t>>&,
        const cortrix::observability::TraceContext*) override {
        return cortrix::Status::Ok();
    }
    cortrix::Status MarkDelete(uint64_t,
                               const cortrix::observability::TraceContext*) override {
        return cortrix::Status::Ok();
    }
    bool Exists(uint64_t) override { return false; }
    bool Exists(uint64_t, const cortrix::observability::TraceContext*) override {
        return false;
    }
    cortrix::Status Snapshot() override { return cortrix::Status::Ok(); }
    cortrix::Status Snapshot(const cortrix::observability::TraceContext*) override {
        return cortrix::Status::Ok();
    }
    cortrix::Status Recover() override { return cortrix::Status::Ok(); }
    cortrix::Status Recover(const cortrix::observability::TraceContext*) override {
        return cortrix::Status::Ok();
    }
    cortrix::Status Shutdown() override { return cortrix::Status::Ok(); }
    cortrix::store::IndexStats GetStats() override {
        return cortrix::store::IndexStats{};
    }
    std::size_t GetMemoryFootprintBytes() const override { return 0; }

private:
    std::vector<std::pair<uint64_t, float>> search_result_;
    bool search_throws_ = false;
};

// ---------------------------------------------------------------------------
// Layer 1b: in-memory CortrixStore. Only block_get / db_handle matter for the
// recall path; everything else is a stub. Seed via Put().
// ---------------------------------------------------------------------------
class FakeDocStore : public cortrix::CortrixStore {
public:
    // Insert/overwrite a block, keyed by block_id.
    void Put(const cortrix::CortrixBlock& b) { blocks_[b.block_id] = b; }

    // Convenience: a block_type=17 doc_summary block.
    static cortrix::CortrixBlock DocSummaryBlock(uint64_t block_id,
                                                 const std::string& doc_id,
                                                 const std::string& summary_text,
                                                 const std::string& metadata_json) {
        cortrix::CortrixBlock b;
        b.block_id = block_id;
        b.doc_id = doc_id;
        b.block_type = 17;  // kBlockDocSummary
        b.content_text = summary_text;
        b.metadata_json = metadata_json;
        // blocks.data is NOT NULL; an empty vector binds as SQL NULL and trips the
        // constraint. Seed a non-empty placeholder payload so block_insert succeeds.
        b.data = {0x01};
        return b;
    }

    int block_get(uint64_t block_id, cortrix::CortrixBlock& out) override {
        auto it = blocks_.find(block_id);
        if (it == blocks_.end()) return -2;  // not found
        out = it->second;
        return 0;
    }

    // --- everything else: minimal stubs ---
    int Open() override { return 0; }
    int Close() override { return 0; }
    int doc_create(cortrix::CortrixDoc&) override { return 0; }
    int doc_get(const std::string&, cortrix::CortrixDoc&) override { return -2; }
    int doc_update_status(const std::string&, cortrix::DocStatus,
                          const std::string&) override {
        return 0;
    }
    int doc_delete(const std::string&) override { return 0; }
    int doc_list_by_status(cortrix::DocStatus,
                           std::vector<cortrix::CortrixDoc>&) override {
        return 0;
    }
    int doc_find_by_source(const std::string&, const std::string&,
                           cortrix::CortrixDoc&) override {
        return -2;
    }
    int doc_find_by_hash(const std::string&, cortrix::CortrixDoc&) override {
        return -2;
    }
    int doc_count(int64_t* c) override {
        if (c) *c = 0;
        return 0;
    }
    int block_insert(cortrix::CortrixBlock& b) override {
        blocks_[b.block_id] = b;
        return 0;
    }
    int block_get_by_doc(const std::string&,
                         std::vector<cortrix::CortrixBlock>&) override {
        return 0;
    }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t* c) override {
        if (c) *c = static_cast<int64_t>(blocks_.size());
        return 0;
    }
    int search_fulltext(const std::string&, int,
                        std::vector<cortrix::SearchResult>&) override {
        return 0;
    }
    int search_metadata(const std::string&, int,
                        std::vector<cortrix::SearchResult>&) override {
        return -1;
    }

private:
    std::map<uint64_t, cortrix::CortrixBlock> blocks_;
};

// ---------------------------------------------------------------------------
// Layer 2: real-pool harness for the endpoint (ExecuteDocDiscovery / Handle).
//
// Reimplements the ns_pool_test_helper pattern but exposes a customizable index
// factory: by default it hands out a FakeDiscoveryIndex (captured for search-result
// / throw control); a test may flip the captured index to throw before calling
// ExecuteDocDiscovery to drive the HNSW-degrade path.
// ---------------------------------------------------------------------------
class DiscoveryPoolHarness {
public:
    explicit DiscoveryPoolHarness(std::filesystem::path data_root)
        : data_root_(std::move(data_root)) {
        std::filesystem::create_directories(data_root_);
        config_.data_root = data_root_.string();
        pool_ = MakePool();
    }

    ~DiscoveryPoolHarness() {
        pool_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_root_, ec);
    }

    DiscoveryPoolHarness(const DiscoveryPoolHarness&) = delete;
    DiscoveryPoolHarness& operator=(const DiscoveryPoolHarness&) = delete;

    cortrix::resource::INamespacePool& ipool() { return *pool_; }
    FakeDiscoveryIndex* fake_index() const { return fake_index_; }

    // Create the per-Unit dir + admit the NS so Acquire/facade hit.
    cortrix::Status Admit(const std::string& ns, size_t est_bytes = 100) {
        std::filesystem::create_directories(data_root_ / (ns + "-u"));
        return pool_->AdmitCreate(ns, est_bytes);
    }

    // Open a scoped facade on `ns` (real store.db) and run `fn(store)`.
    template <typename Fn>
    void WithStore(const std::string& ns, Fn&& fn) {
        cortrix::resource::NamespaceFacade f(*pool_, ns);
        ASSERT_TRUE(f.Acquire().ok());
        fn(f.store());
    }

    // Seed a block_type=17 doc_summary row on the real store.db (returns block_id).
    uint64_t SeedDocSummaryBlock(const std::string& ns, const std::string& doc_id,
                                 const std::string& summary_text,
                                 const std::string& metadata_json) {
        uint64_t id = next_block_id_++;
        WithStore(ns, [&](cortrix::CortrixStore& store) {
            // blocks.doc_id is a FK → documents.doc_id; the parent document row must
            // exist first. doc_create honors a pre-set doc_id (only mints when empty).
            // Ignore the result so re-seeding blocks for the same doc is idempotent
            // (a duplicate-PK INSERT fails harmlessly; the row already exists).
            cortrix::CortrixDoc doc;
            doc.doc_id = doc_id;
            doc.source_type = "test";
            store.doc_create(doc);
            cortrix::CortrixBlock b = FakeDocStore::DocSummaryBlock(
                id, doc_id, summary_text, metadata_json);
            ASSERT_EQ(store.block_insert(b), 0);
        });
        return id;
    }

    // Create the per-Unit doc_fts5_index virtual table + insert one row, mirroring
    // the DocSummarySchemaProvider column order (filename, doc_title, topics, authors).
    void CreateFts5Table(const std::string& ns, const std::string& doc_id,
                         const std::string& filename, const std::string& doc_title,
                         const std::string& topics, const std::string& authors) {
        WithStore(ns, [&](cortrix::CortrixStore& store) {
            sqlite3* db = store.db_handle();
            ASSERT_NE(db, nullptr);
            const char* ddl =
                "CREATE VIRTUAL TABLE IF NOT EXISTS doc_fts5_index USING fts5("
                "doc_id UNINDEXED, filename, doc_title, "
                "topics_rule_extracted, authors)";
            char* err = nullptr;
            ASSERT_EQ(sqlite3_exec(db, ddl, nullptr, nullptr, &err), SQLITE_OK)
                << (err ? err : "");
            const char* ins =
                "INSERT INTO doc_fts5_index(doc_id, filename, doc_title, "
                "topics_rule_extracted, authors) VALUES (?,?,?,?,?)";
            sqlite3_stmt* st = nullptr;
            ASSERT_EQ(sqlite3_prepare_v2(db, ins, -1, &st, nullptr), SQLITE_OK);
            sqlite3_bind_text(st, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, filename.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, doc_title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, topics.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 5, authors.c_str(), -1, SQLITE_TRANSIENT);
            ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
            sqlite3_finalize(st);
        });
    }

private:
    cortrix::catalog::UnitDescriptor Unit(const std::string& unit_id) {
        cortrix::catalog::UnitDescriptor u;
        u.unit_id = unit_id;
        u.tenant_id = "t1";
        return u;
    }

    cortrix::resource::WriteCoordinatorFactory MakeRealCoordFactory() {
        return [](const std::string& data_dir,
                  const cortrix::store::WriteCoordinatorConfig& cfg,
                  cortrix::store::IVectorStore* vs,
                  cortrix::store::IMetadataStore* ms,
                  cortrix::store::IBlobStore* bs)
                   -> cortrix::Result<std::unique_ptr<cortrix::store::WriteCoordinator>> {
            auto coord =
                std::make_unique<cortrix::store::WriteCoordinator>(data_dir, cfg, vs, ms, bs);
            cortrix::Status init = coord->Init();
            if (!init.ok()) return init;
            return cortrix::Result<std::unique_ptr<cortrix::store::WriteCoordinator>>(
                std::move(coord));
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
                auto idx = std::make_unique<FakeDiscoveryIndex>();
                fake_index_ = idx.get();
                return cortrix::Result<std::unique_ptr<cortrix::store::IIndex>>(
                    std::move(idx));
            }));
        return std::make_unique<cortrix::resource::DefaultNamespacePool>(
            &index_factory_, MakeRealCoordFactory(), &ns_router_, &unit_router_,
            config_);
    }

    // ----- mocked catalog routers (only GetActiveUnit / Open are exercised) -----
    class MockNSRouter : public cortrix::catalog::INSRouter {
    public:
        MOCK_METHOD((cortrix::Result<std::vector<cortrix::catalog::UnitDescriptor>>),
                    LookupUnits, (const std::string&), (const, override));
        MOCK_METHOD((cortrix::Result<cortrix::catalog::UnitDescriptor>), GetActiveUnit,
                    (const std::string&), (const, override));
        MOCK_METHOD((cortrix::Result<cortrix::catalog::NSMetadata>), GetNamespace,
                    (const std::string&), (const, override));
        MOCK_METHOD((cortrix::Result<cortrix::catalog::BatchResult<std::string>>),
                    ListNamespaces, (const cortrix::catalog::ListNamespacesOptions&),
                    (const, override));
        MOCK_METHOD(cortrix::Status, CreateNamespace,
                    (const cortrix::catalog::NSMetadata&), (override));
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
                    (const std::string&, const cortrix::catalog::UnitStats&),
                    (override));
        MOCK_METHOD((cortrix::Result<bool>), ShouldSeal, (const std::string&),
                    (const, override));
    };
    class MockIndexFactory : public cortrix::store::IIndexFactory {
    public:
        MOCK_METHOD((cortrix::Result<std::unique_ptr<cortrix::store::IIndex>>), Create,
                    (const std::string&, const cortrix::store::IndexConfig&),
                    (override));
        MOCK_METHOD((cortrix::Result<std::unique_ptr<cortrix::store::IIndex>>), Open,
                    (const std::string&), (override));
    };

    std::filesystem::path data_root_;
    ::testing::NiceMock<MockNSRouter> ns_router_;
    ::testing::NiceMock<MockUnitRouter> unit_router_;
    ::testing::NiceMock<MockIndexFactory> index_factory_;
    FakeDiscoveryIndex* fake_index_ = nullptr;
    cortrix::resource::NamespacePoolConfig config_;
    std::unique_ptr<cortrix::resource::DefaultNamespacePool> pool_;
    uint64_t next_block_id_ = 1000;
};

}  // namespace cortrix::doc_summary::test
