#pragma once
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "cortrix/store/chunk_store.h"

namespace cortrix::store {

/// Shared gmock double for ChunkStore (scaffolding D2-pre-8) so F02 / F04 / F37
/// can unit-test their reverse-lookup logic without F08/F09.
class MockChunkStore : public ChunkStore {
public:
    MOCK_METHOD(Result<ChunkRecord>, Get, (const std::string& child_id), (override));
    MOCK_METHOD(std::vector<ChunkRecord>, GetBatch,
                (const std::vector<std::string>& child_ids, std::vector<std::string>* missing_ids),
                (override));
    MOCK_METHOD(std::vector<ChunkRecord>, GetChunksByDocId, (const std::string& doc_id), (override));
};

}  // namespace cortrix::store
