#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <memory>
#include <utility>
#include <vector>

#include "cortrix/query/vector_searcher.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/iindex.h"
#include "mocks/mock_embedder.h"

using ::testing::_;
using ::testing::Return;

namespace cortrix {
namespace {

// gmock IIndex (F01) — VectorSearcher now takes a store::IIndex& (D3.5 wire⑤c,
// was the MVP CortrixVectorIndex). Only Search is exercised; the rest are stubbed
// so the abstract base is concrete. IIndex::Search returns its hits directly
// (std::vector<{block_id, distance}>), there is NO rc — empty = no match.
class MockIIndex : public cortrix::store::IIndex {
public:
    MOCK_METHOD(Status, AddPoint,
                (const float*, uint64_t, const cortrix::observability::TraceContext*), (override));
    MOCK_METHOD(Status, AddPoints,
                ((const std::vector<std::pair<const float*, uint64_t>>&),
                 const cortrix::observability::TraceContext*), (override));
    MOCK_METHOD(Status, MarkDelete,
                (uint64_t, const cortrix::observability::TraceContext*), (override));
    MOCK_METHOD((std::vector<std::pair<uint64_t, float>>), Search,
                (const float*, int, int, const cortrix::observability::TraceContext*), (override));
    MOCK_METHOD(bool, Exists, (uint64_t), (override));
    MOCK_METHOD(Status, Snapshot, (), (override));
    MOCK_METHOD(Status, Recover, (), (override));
    MOCK_METHOD(Status, Shutdown, (), (override));
    MOCK_METHOD(cortrix::store::IndexStats, GetStats, (), (override));
    MOCK_METHOD(std::size_t, GetMemoryFootprintBytes, (), (const, override));
};

using Hits = std::vector<std::pair<uint64_t, float>>;

// ============================================================
// Real VectorSearcher tests using stub OnnxEmbedder + mock IIndex.
// These exercise VectorSearcher::Search() over the F01 IIndex API.
// ============================================================

class VectorSearcherRealTest : public ::testing::Test {
protected:
    void SetUp() override {
        // The Phase-1 stub OnnxEmbedder returns deterministic fake vectors.
        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
    }

    std::unique_ptr<OnnxEmbedder> embedder_;
    MockIIndex mock_index_;
};

// --- Core path: successful search ---

TEST_F(VectorSearcherRealTest, Search_Success_ReturnsItems) {
    // top_k=10 -> oversampled search_k=30 (the 2nd Search arg).
    EXPECT_CALL(mock_index_, Search(_, 30, _, _))
        .WillOnce(Return(Hits{{100, 0.1f}, {200, 0.5f}}));

    VectorSearcher searcher(mock_index_, *embedder_);
    auto result = searcher.Search("test query", 10, 5000000);  // 5s timeout

    EXPECT_EQ(result.route_name, "vector");
    EXPECT_EQ(result.status, RouteStatus::kSuccess);
    ASSERT_EQ(result.items.size(), 2u);

    // Check distance-to-similarity conversion: 1/(1+distance)
    EXPECT_NEAR(result.items[0].raw_score, 1.0f / (1.0f + 0.1f), 0.001f);
    EXPECT_EQ(result.items[0].block_id, 100u);
    EXPECT_NEAR(result.items[1].raw_score, 1.0f / (1.0f + 0.5f), 0.001f);
    EXPECT_EQ(result.items[1].block_id, 200u);
    EXPECT_GT(result.latency_us, 0);
}

TEST_F(VectorSearcherRealTest, Search_EmptyResults) {
    // top_k=5 -> search_k=15. Empty hits = no match (not an error).
    EXPECT_CALL(mock_index_, Search(_, 15, _, _)).WillOnce(Return(Hits{}));

    VectorSearcher searcher(mock_index_, *embedder_);
    auto result = searcher.Search("find nothing", 5, 5000000);

    EXPECT_EQ(result.status, RouteStatus::kSuccess);
    EXPECT_TRUE(result.items.empty());
}

// NOTE: the old Search_IndexError_ReturnsError case is removed — F01
// IIndex::Search has no rc/error return (empty = no hits), so the vector route
// can no longer fail at the index layer. The only remaining error source is
// embedding (below).

// --- Error path: embedding fails ---

TEST_F(VectorSearcherRealTest, Search_EmbeddingFails_ReturnsError) {
    // Create an uninitialized embedder (session_ == nullptr) to trigger failure.
    OnnxEmbedder bad_embedder("bad_model.onnx", 128);
    // Do NOT call Init() -> Embed() will return Internal error.

    VectorSearcher searcher(mock_index_, bad_embedder);
    auto result = searcher.Search("test", 10, 5000000);

    EXPECT_EQ(result.route_name, "vector");
    EXPECT_EQ(result.status, RouteStatus::kError);
    EXPECT_TRUE(result.error_message.find("embedding failed") != std::string::npos);
    EXPECT_TRUE(result.items.empty());
    EXPECT_GE(result.latency_us, 0);  // May be 0 on fast machines for early error path
}

// --- Oversampling: search_k = top_k * 3 ---

TEST_F(VectorSearcherRealTest, Search_Oversampling_TopK1) {
    EXPECT_CALL(mock_index_, Search(_, 3, _, _)).WillOnce(Return(Hits{{1, 0.1f}}));

    VectorSearcher searcher(mock_index_, *embedder_);
    auto result = searcher.Search("test", 1, 5000000);
    EXPECT_EQ(result.status, RouteStatus::kSuccess);
    EXPECT_EQ(result.items.size(), 1u);
}

TEST_F(VectorSearcherRealTest, Search_Oversampling_TopK100) {
    EXPECT_CALL(mock_index_, Search(_, 300, _, _)).WillOnce(Return(Hits{}));

    VectorSearcher searcher(mock_index_, *embedder_);
    auto result = searcher.Search("test", 100, 5000000);
    EXPECT_EQ(result.status, RouteStatus::kSuccess);
}

// --- Latency is always recorded ---

TEST_F(VectorSearcherRealTest, Search_LatencyAlwaysRecorded) {
    EXPECT_CALL(mock_index_, Search(_, _, _, _)).WillOnce(Return(Hits{{1, 0.1f}}));

    VectorSearcher searcher(mock_index_, *embedder_);
    auto result = searcher.Search("test", 10, 5000000);
    EXPECT_GE(result.latency_us, 0);
}

// --- doc_id is always empty (PostFilter resolves it) ---

TEST_F(VectorSearcherRealTest, Search_DocIdAlwaysEmpty) {
    EXPECT_CALL(mock_index_, Search(_, _, _, _)).WillOnce(Return(Hits{{42, 1.5f}}));

    VectorSearcher searcher(mock_index_, *embedder_);
    auto result = searcher.Search("test", 10, 5000000);
    ASSERT_EQ(result.items.size(), 1u);
    EXPECT_TRUE(result.items[0].doc_id.empty());  // IIndex hits don't carry doc_id
    EXPECT_EQ(result.items[0].chunk_index, 0);
}

// --- Multiple results with score ordering ---

TEST_F(VectorSearcherRealTest, Search_MultipleResults_ScoreConversion) {
    // distance=0 -> score=1.0, distance=1.0 -> score=0.5, distance=4.0 -> score=0.2
    EXPECT_CALL(mock_index_, Search(_, _, _, _))
        .WillOnce(Return(Hits{{1, 0.0f}, {2, 1.0f}, {3, 4.0f}}));

    VectorSearcher searcher(mock_index_, *embedder_);
    auto result = searcher.Search("test", 10, 5000000);

    ASSERT_EQ(result.items.size(), 3u);
    EXPECT_NEAR(result.items[0].raw_score, 1.0f, 0.001f);
    EXPECT_NEAR(result.items[1].raw_score, 0.5f, 0.001f);
    EXPECT_NEAR(result.items[2].raw_score, 0.2f, 0.001f);
}

// ============================================================
// Standalone helpers (no vector index dependency)
// ============================================================

TEST(VectorSearcherTest, SearchSuccess) {
    auto fake_embedder = std::make_unique<testing::FakeEmbedder>(768);
    EmbeddingResult embed_result;
    Status status = fake_embedder->Embed("test query", &embed_result);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(embed_result.dim, 768);
    EXPECT_EQ(embed_result.vector.size(), 768u);
}

TEST(VectorSearcherTest, DistanceToSimilarityConversion) {
    EXPECT_NEAR(1.0f / (1.0f + 0.0f), 1.0f, 0.001f);
    EXPECT_NEAR(1.0f / (1.0f + 1.0f), 0.5f, 0.001f);
    EXPECT_NEAR(1.0f / (1.0f + 4.0f), 0.2f, 0.001f);
}

}  // namespace
}  // namespace cortrix
