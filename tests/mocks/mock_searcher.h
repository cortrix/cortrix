#pragma once
#include <gmock/gmock.h>
#include "cortrix/query/vector_searcher.h"
#include "cortrix/query/bm25_searcher.h"

namespace cortrix {
namespace testing {

// VectorSearcher and BM25Searcher are concrete classes, not virtual interfaces.
// For testing QueryPipeline, mock the underlying dependencies:
//
//   MockVectorIndex mock_vec_idx;
//   MockCortrixStore mock_store;
//   FakeEmbedder fake_embedder(1024);
//
// Then construct real VectorSearcher/BM25Searcher with mocked dependencies.
// Set expectations on mock_vec_idx.search() and mock_store.search_fulltext()
// to control search results in tests.

}  // namespace testing
}  // namespace cortrix
