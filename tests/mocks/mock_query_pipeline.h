#pragma once
#include <gmock/gmock.h>
#include "cortrix/query/query_pipeline.h"

namespace cortrix {
namespace testing {

// Note: QueryPipeline is not a virtual interface, so we cannot directly mock it.
// For testing, we provide a fake/stub that can be used in integration tests.
// Feature agents should inject dependencies (VectorSearcher, BM25Searcher, etc.)
// and mock those individual components instead.

// If needed, tests can create QueryPipeline with mock searchers:
//   MockVectorIndex vec_idx;
//   MockCortrixStore store;
//   OnnxEmbedder embedder(...);
//   VectorSearcher vec_searcher(vec_idx, embedder);
//   BM25Searcher bm25_searcher(store);
//   RRFFusion fusion;
//   IntentClassifier classifier(llm_config);
//   PostFilter filter(store);
//   DegradationManager degradation;
//   QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
//                          classifier, filter, degradation);

}  // namespace testing
}  // namespace cortrix
