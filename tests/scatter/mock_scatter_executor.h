#pragma once
#include <gmock/gmock.h>

#include <string>

#include "cortrix/query/i_scatter_executor.h"

namespace cortrix::query {

/// Shared gmock double for IScatterExecutor. Lets ScatterGather be
/// unit-tested (single/multi-NS split, dual-timeout, gather) without a real
/// SingleUnitExecutor / pipeline / reranker. Phase 2's multi-Unit router reuses it.
class MockIScatterExecutor : public IScatterExecutor {
public:
    MOCK_METHOD(retrieval::NamespaceQueryResult, ExecuteForNamespace,
                (const QueryContext&, const std::string&, float), (override));
};

}  // namespace cortrix::query
