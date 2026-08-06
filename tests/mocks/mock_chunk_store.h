#pragma once
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "cortrix/store/chunk_store.h"

namespace cortrix::store {

/// Shared gmock double for ChunkStore (scaffolding) so reranker / cross-NS query / CRAG
/// can unit-test their reverse-lookup logic without META block/block header.
class MockChunkStore : public ChunkStore {
public:
    MOCK_METHOD(Result<ChunkRecord>, Get, (const std::string& child_id), (override));
    MOCK_METHOD(std::vector<ChunkRecord>, GetBatch,
                (const std::vector<std::string>& child_ids, std::vector<std::string>* missing_ids),
                (override));
    MOCK_METHOD(std::vector<ChunkRecord>, GetChunksByDocId, (const std::string& doc_id), (override));
};

}  // namespace cortrix::store
