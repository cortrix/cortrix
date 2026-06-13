#pragma once
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "cortrix/reranker.h"

namespace cortrix::reranker {

/// Shared gmock double for IReranker (F02 §7.2). Lets the query pipeline / F04 /
/// F37 unit-test their reranker integration without the real ONNX model.
///
/// NOTE: F02 §7.2 lists Score/ScoreBatch/Name; the interface also has a pure
/// virtual Rerank (added v1.0.3, V5 decision #4A), which MUST be mocked too for the
/// double to be instantiable.
class MockReranker : public IReranker {
public:
    MOCK_METHOD(float, Score, (const char* query, const char* passage), (override));
    MOCK_METHOD(std::vector<float>, ScoreBatch,
                (const char* query, const std::vector<const char*>& passages), (override));
    MOCK_METHOD(std::vector<retrieval::RankedChunk>, Rerank,
                (const std::vector<retrieval::ScoredResult>& candidates,
                 const std::string& query),
                (override));
    MOCK_METHOD(const char*, Name, (), (const, override));
};

}  // namespace cortrix::reranker
