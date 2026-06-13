#pragma once
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "cortrix/chunker/types.h"
#include "cortrix/store/parent_chunk_store.h"

namespace cortrix::store {

/// Shared gmock double for ParentChunkStore (F34 § 2.5) so F38 HyPEEnricher /
/// F41 doc-summary can unit-test their parent reverse-lookup logic without the
/// SQLite store. Parallel to MockChunkStore (child-level).
class MockParentChunkStore : public ParentChunkStore {
public:
    MOCK_METHOD(Result<cortrix::chunker::ParentChunk>, GetParent,
                (const std::string& parent_id), (override));
    MOCK_METHOD(Result<std::vector<cortrix::chunker::ParentChunk>>, BulkGetParents,
                (const std::vector<std::string>& parent_ids), (override));
};

}  // namespace cortrix::store
