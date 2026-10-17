#pragma once
#include <chrono>
#include <string>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/query/cross_ns_error.h"
#include "cortrix/retrieval/cross_ns_types.h"

namespace cortrix::query {

/// The row a namespace gets when the request's time ran out before it answered:
/// CX_ERR_NS_TIMEOUT with the category, retryability and retry hint of the error
/// table, and the time since `start` as its latency. The join writes it for a
/// namespace it stopped waiting for or never started; the executor returns it when it
/// stops at a stage boundary because the request's deadline has passed. One builder,
/// so the two can never disagree on the row's fields.
inline retrieval::NamespaceQueryResult MakeNamespaceTimeoutRow(
    const std::string& namespace_id, std::chrono::steady_clock::time_point start) {
    const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kNsTimeout);
    retrieval::NamespaceQueryResult row;
    row.namespace_id = namespace_id;
    row.error_code = info.cx_code;
    row.error_category = agent_friendly::ToString(info.category);
    row.retryable = info.retryable;
    row.retry_after_ms = info.retry_after_ms.value_or(0);
    row.latency_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
    return row;
}

/// Whether a namespace's row means the overall deadline cut the query off, so the
/// response also carries the top-level CX_ERR_SCATTER_TIMEOUT: the row is a timeout
/// row and the join saw it at or after `overall`. `ready_at` is the join's own clock
/// reading, taken right after its wait returned. A row the executor itself reported
/// as timed out counts the same as one the join stopped waiting for.
inline bool CutByOverallDeadline(const retrieval::NamespaceQueryResult& row,
                                 std::chrono::steady_clock::time_point ready_at,
                                 std::chrono::steady_clock::time_point overall) {
    return row.error_code == GetCrossNsErrorInfo(CrossNsErrorCode::kNsTimeout).cx_code &&
           ready_at >= overall;
}

}  // namespace cortrix::query
