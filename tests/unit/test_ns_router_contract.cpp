#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

// S2.1 coverage (interface/contract half): catalog value types + the INSRouter
// abstract contract. The DefaultINSRouter implementation is S2.2 — here we only
// lock the published surface (signatures, return types, type defaults).
namespace cortrix::catalog {
namespace {

// --- value type round-trips / defaults ---------------------------

TEST(CatalogTypesTest, UnitStateRoundTrip) {
    for (UnitState s : {UnitState::kActive, UnitState::kSealing, UnitState::kSealed,
                        UnitState::kArchived, UnitState::kMigrating}) {
        EXPECT_EQ(UnitStateFromString(ToString(s)), s);
    }
    // unknown / Phase-1 conservative default.
    EXPECT_EQ(UnitStateFromString("bogus"), UnitState::kActive);
    EXPECT_STREQ(ToString(UnitState::kMigrating), "migrating");
}

TEST(CatalogTypesTest, IndexTypeRoundTrip) {
    for (IndexType t : {IndexType::kHnsw, IndexType::kBruteForce,
                        IndexType::kQuantizedHnsw}) {
        EXPECT_EQ(IndexTypeFromString(ToString(t)), t);
    }
    EXPECT_EQ(IndexTypeFromString("bogus"), IndexType::kHnsw);
    EXPECT_STREQ(ToString(IndexType::kQuantizedHnsw), "quantized_hnsw");
}

TEST(CatalogTypesTest, UnitDescriptorPhase1Defaults) {
    UnitDescriptor u;
    EXPECT_EQ(u.state, UnitState::kActive);
    EXPECT_EQ(u.index_type, IndexType::kHnsw);
    EXPECT_EQ(u.owner_node_id, "local");
    EXPECT_FALSE(u.sealed_at.has_value());
    EXPECT_FALSE(u.archived_at.has_value());
    EXPECT_EQ(u.vector_count, 0);
    EXPECT_EQ(u.seal_threshold_vectors, INT64_MAX);
    EXPECT_EQ(u.seal_threshold_bytes, INT64_MAX);
    EXPECT_EQ(u.seal_idle_days, INT32_MAX);
}

TEST(CatalogTypesTest, NSMetadataPhase1Defaults) {
    NSMetadata ns;
    EXPECT_EQ(ns.isolation_mode, "shared");
    EXPECT_EQ(ns.visibility, "tenant");
    EXPECT_EQ(ns.status, "active");
    // 11 *_config blobs default to "unset" (column default '{}').
    EXPECT_FALSE(ns.reranker_config.has_value());
    EXPECT_FALSE(ns.sparse_config.has_value());
    EXPECT_FALSE(ns.doc_summary_config.has_value());
}

TEST(CatalogTypesTest, BatchResultDefaults) {
    BatchResult<std::string> br;
    EXPECT_TRUE(br.results.empty());
    EXPECT_FLOAT_EQ(br.meta.coverage_ratio, 1.0f);
    EXPECT_EQ(br.meta.api_version, "v1");
    EXPECT_TRUE(br.meta.bloom_filter_ready);
    EXPECT_FALSE(br.meta.node_id.has_value());
}

// --- INSRouter contract surface ---------------------------------------------

// A throwaway implementation that compiles against all 6 INSRouter methods.
// If a signature/return type drifts, this fails to compile — that is the point:
// it pins the contract the L0 freeze review locks. Value methods return Result<T>,
// the two lifecycle methods return Status (F-FREEZE-1: no Result<void>).
class FakeNSRouter : public INSRouter {
public:
    Result<std::vector<UnitDescriptor>> LookupUnits(
        const std::string&) const override {
        return std::vector<UnitDescriptor>{UnitDescriptor{}};
    }
    Result<UnitDescriptor> GetActiveUnit(const std::string&) const override {
        return UnitDescriptor{};
    }
    Result<NSMetadata> GetNamespace(const std::string&) const override {
        return NSMetadata{};
    }
    Result<BatchResult<std::string>> ListNamespaces(
        const ListNamespacesOptions&) const override {
        return BatchResult<std::string>{};
    }
    Status CreateNamespace(const NSMetadata&) override { return Status::Ok(); }
    Status DeleteNamespace(const std::string&) override { return Status::Ok(); }
};

// Compile-time: the lifecycle methods return Status, the value methods Result<T>.
static_assert(
    std::is_same_v<decltype(std::declval<FakeNSRouter>().CreateNamespace(
                       std::declval<NSMetadata>())),
                   Status>,
    "CreateNamespace must return Status (F-FREEZE-1: no Result<void>)");
static_assert(
    std::is_same_v<decltype(std::declval<FakeNSRouter>().DeleteNamespace(
                       std::declval<std::string>())),
                   Status>,
    "DeleteNamespace must return Status");

TEST(NSRouterContractTest, IsAbstractInterface) {
    EXPECT_TRUE(std::is_abstract_v<INSRouter>);
    // a virtual dtor so deleting via base is safe.
    EXPECT_TRUE(std::has_virtual_destructor_v<INSRouter>);
}

TEST(NSRouterContractTest, FakeImplSatisfiesContract) {
    std::unique_ptr<INSRouter> r = std::make_unique<FakeNSRouter>();

    auto units = r->LookupUnits("ns1");
    ASSERT_TRUE(units.ok());
    EXPECT_EQ(units.value().size(), 1u);

    EXPECT_TRUE(r->GetActiveUnit("ns1").ok());
    EXPECT_TRUE(r->GetNamespace("ns1").ok());

    auto list = r->ListNamespaces();
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(list.value().meta.api_version, "v1");

    EXPECT_TRUE(r->CreateNamespace(NSMetadata{}).ok());
    EXPECT_TRUE(r->DeleteNamespace("ns1").ok());
}

// The error path: a Result<T> built from an error Status reports !ok() and the
// CX_ERR_ identity is reachable through the catalog error registry (smoke-level;
// full mapping is exercised in test_catalog_error.cpp).
TEST(NSRouterContractTest, ResultErrorPathCarriesStatus) {
    Result<UnitDescriptor> err = Status::NotFound("CX_ERR_NS_NOT_FOUND: ns missing");
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace cortrix::catalog
