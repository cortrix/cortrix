#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/store/iindex_factory.h"

namespace cortrix::store {
namespace {

using ::testing::_;
using ::testing::Return;

// Minimal IIndex stand-in so factory tests can hand back a working instance.
class FakeIndex : public IIndex {
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
        return {{1, 0.5f}};
    }
    bool Exists(uint64_t) override { return true; }
    Status Snapshot() override { return Status::Ok(); }
    Status Recover() override { return Status::Ok(); }
    Status Shutdown() override { return Status::Ok(); }
    IndexStats GetStats() override { return IndexStats{}; }
    std::size_t GetMemoryFootprintBytes() const override { return 0; }
};

class MockIndexFactory : public IIndexFactory {
public:
    MOCK_METHOD((Result<std::unique_ptr<IIndex>>), Create,
                (const std::string&, const IndexConfig&), (override));
    MOCK_METHOD((Result<std::unique_ptr<IIndex>>), Open, (const std::string&), (override));
};

// Mirrors catalog OpenUnitIndex: hold an IIndexFactory*, Open(unit_path), use IIndex.
TEST(IIndexFactoryTest, F12StyleOpenUnitIndexThroughMock) {
    MockIndexFactory factory;
    EXPECT_CALL(factory, Open("/data/units/unit_legal_0")).WillOnce([](const std::string&) {
        return Result<std::unique_ptr<IIndex>>(std::make_unique<FakeIndex>());
    });

    IIndexFactory* f = &factory;  // Catalog holds only the interface pointer
    auto opened = f->Open("/data/units/unit_legal_0");
    ASSERT_TRUE(opened.ok());
    ASSERT_NE(opened.value(), nullptr);
    EXPECT_TRUE(opened.value()->Exists(1));
}

TEST(IIndexFactoryTest, OpenErrorPropagatesAsResultStatus) {
    MockIndexFactory factory;
    EXPECT_CALL(factory, Open(_)).WillOnce([](const std::string&) {
        return Result<std::unique_ptr<IIndex>>(Status::NotFound("no such unit"));
    });

    auto opened = factory.Open("/data/units/missing");
    EXPECT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), StatusCode::kNotFound);
}

TEST(IIndexFactoryTest, CreateUsesIndexConfigDefaults) {
    MockIndexFactory factory;
    IndexConfig cfg;
    EXPECT_EQ(cfg.type, IndexType::kPhnsw);
    EXPECT_EQ(cfg.dim, 1024);
    EXPECT_CALL(factory, Create("/data/units/new", _)).WillOnce([](const std::string&,
                                                                   const IndexConfig&) {
        return Result<std::unique_ptr<IIndex>>(std::make_unique<FakeIndex>());
    });
    auto created = factory.Create("/data/units/new", cfg);
    EXPECT_TRUE(created.ok());
}

// The real index build replaced the skeleton: PhnswIndexFactory now builds a
// working PHnsw IIndex. (The skeleton version of this test asserted kUnavailable.)
TEST(IIndexFactoryTest, PhnswFactoryCreatesWorkingIndex) {
    namespace fs = std::filesystem;
    const fs::path unit_dir = fs::temp_directory_path() / "cortrix_phnsw_factory_create";
    fs::remove_all(unit_dir);

    PhnswIndexFactory factory;
    IndexConfig cfg;
    cfg.dim = 8;  // small dim keeps the test cheap
    auto created = factory.Create(unit_dir.string(), cfg);
    ASSERT_TRUE(created.ok());
    ASSERT_NE(created.value(), nullptr);

    // The Unit data directory is provisioned and the index is empty + ready.
    EXPECT_TRUE(fs::exists(unit_dir));
    auto stats = created.value()->GetStats();
    EXPECT_EQ(stats.vector_count, 0u);
    EXPECT_EQ(stats.dim, 8);

    fs::remove_all(unit_dir);
}

TEST(IIndexFactoryTest, PhnswFactoryOpensExistingUnit) {
    namespace fs = std::filesystem;
    const fs::path unit_dir = fs::temp_directory_path() / "cortrix_phnsw_factory_open";
    fs::remove_all(unit_dir);

    PhnswIndexFactory factory;
    // Open over an empty Unit succeeds (S1: nothing to recover yet).
    auto opened = factory.Open(unit_dir.string());
    ASSERT_TRUE(opened.ok());
    ASSERT_NE(opened.value(), nullptr);
    EXPECT_FALSE(opened.value()->Exists(123));  // empty index

    fs::remove_all(unit_dir);
}

// D35-NEW-01: Open() must reconstruct the index with the Unit's persisted dim/M
// (read from the newest snapshot's .meta footer), not the 1024 default. A Unit
// created at dim=8 and snapshotted must reopen as dim=8 and stay searchable —
// before D35-NEW-01 Open() built a 1024-dim L2Space over an 8-dim snapshot.
TEST(IIndexFactoryTest, PhnswFactoryOpenReadsPersistedDim) {
    namespace fs = std::filesystem;
    const fs::path unit_dir = fs::temp_directory_path() / "cortrix_phnsw_factory_reopen_dim";
    fs::remove_all(unit_dir);

    PhnswIndexFactory factory;
    // 1. Create at a non-default dim/M, add a vector, snapshot it (writes .meta dim=8).
    {
        IndexConfig cfg;
        cfg.dim = 8;
        cfg.M = 24;  // non-default M too
        auto created = factory.Create(unit_dir.string(), cfg);
        ASSERT_TRUE(created.ok());
        auto& idx = *created.value();
        std::vector<float> v(8, 0.25f);
        ASSERT_TRUE(idx.AddPoint(v.data(), 42, nullptr).ok());
        ASSERT_TRUE(idx.Snapshot().ok());
    }

    // 2. Reopen via the factory — dim/M come from the .meta footer, not defaults.
    auto opened = factory.Open(unit_dir.string());
    ASSERT_TRUE(opened.ok());
    ASSERT_NE(opened.value(), nullptr);
    auto& reopened = *opened.value();
    EXPECT_EQ(reopened.GetStats().dim, 8);  // persisted dim, not the 1024 default

    // The 8-dim query matches the persisted vector (would break on a dim mismatch).
    std::vector<float> q(8, 0.25f);
    auto hits = reopened.Search(q.data(), 5, -1, nullptr);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].first, 42u);

    fs::remove_all(unit_dir);
}

TEST(ResultTest, HoldsValueOrError) {
    Result<int> good(42);
    EXPECT_TRUE(good.ok());
    EXPECT_EQ(good.value(), 42);

    Result<int> bad(Status::InvalidArgument("nope"));
    EXPECT_FALSE(bad.ok());
    EXPECT_EQ(bad.status().code(), StatusCode::kInvalidArgument);
}

TEST(ResultTest, MoveOnlyValueWorks) {
    Result<std::unique_ptr<int>> r(std::make_unique<int>(7));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r.value(), 7);
}

}  // namespace
}  // namespace cortrix::store
