#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/store/chunk_store.h"
#include "mock_chunk_store.h"

namespace cortrix::store {
namespace {

using ::testing::_;
using ::testing::Return;

ChunkRecord MakeChunk(std::string child, std::string parent, std::string text, int idx) {
    ChunkRecord r;
    r.child_id = std::move(child);
    r.parent_id = std::move(parent);
    r.content = std::move(text);
    r.chunk_index = idx;
    return r;
}

TEST(ChunkStoreTest, GetReturnsRecordThroughInterface) {
    MockChunkStore mock;
    EXPECT_CALL(mock, Get("01J0CHILD")).WillOnce([](const std::string&) {
        return Result<ChunkRecord>(MakeChunk("01J0CHILD", "doc_1", "hello world", 3));
    });

    ChunkStore* store = &mock;  // F02 holds only the interface
    auto r = store->Get("01J0CHILD");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().parent_id, "doc_1");
    EXPECT_EQ(r.value().content, "hello world");
    EXPECT_EQ(r.value().chunk_index, 3);
}

TEST(ChunkStoreTest, GetMissingReturnsNotFound) {
    MockChunkStore mock;
    EXPECT_CALL(mock, Get(_)).WillOnce([](const std::string&) {
        return Result<ChunkRecord>(Status::NotFound("CX_ERR_CHUNK_NOT_FOUND"));
    });

    auto r = mock.Get("missing");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
}

TEST(ChunkStoreTest, GetBatchReportsMissing) {
    MockChunkStore mock;
    EXPECT_CALL(mock, GetBatch(_, _))
        .WillOnce([](const std::vector<std::string>& ids, std::vector<std::string>* missing) {
            std::vector<ChunkRecord> out;
            for (const auto& id : ids) {
                if (id == "absent") {
                    if (missing) missing->push_back(id);
                } else {
                    out.push_back(MakeChunk(id, "doc_1", "c", 0));
                }
            }
            return out;
        });

    std::vector<std::string> missing;
    auto out = mock.GetBatch({"a", "absent", "b"}, &missing);
    EXPECT_EQ(out.size(), 2u);
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0], "absent");
}

TEST(ChunkStoreTest, GetChunksByDocIdAscendingByIndex) {
    MockChunkStore mock;
    EXPECT_CALL(mock, GetChunksByDocId("doc_1")).WillOnce(Return(std::vector<ChunkRecord>{
        MakeChunk("c0", "doc_1", "first", 0),
        MakeChunk("c1", "doc_1", "second", 1),
    }));

    auto chunks = mock.GetChunksByDocId("doc_1");
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0].chunk_index, 0);
    EXPECT_EQ(chunks[1].chunk_index, 1);
}

}  // namespace
}  // namespace cortrix::store
