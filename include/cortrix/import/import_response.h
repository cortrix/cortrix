#pragma once
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/import/import_types.h"

namespace cortrix::import {

/// Agent-friendly response serialization for F16a (§5.1 / §5.2 / §5.3). These build
/// the exact wire bodies the SDK + OpenAPI contract on, so the schema is
/// the single source of truth even though the real HTTP routing is wired in D3.5.

/// Render a system_clock time point to RFC3339/ISO-8601 UTC ("...Z"). The shared
/// idiom (reranker_status / memory_writer): seconds granularity, "%Y-%m-%dT%H:%M:%SZ".
std::string ToIso8601Utc(std::chrono::system_clock::time_point tp);

/// §5.1 success body for POST /api/v1/import/database. `estimated_rows` is the
/// SELECT COUNT(*) pre-estimate (R5); `progress_endpoint` / `cancel_endpoint` are
/// the §5.1 self-links the Agent follows. error is always null here.
nlohmann::json BuildImportStartedResponse(const ImportTaskProgress& task,
                                          const ConnectionRefId& connection_ref,
                                          int64_t estimated_rows);

/// §5.2 progress body for GET /api/v1/import/tasks/{task_id}/progress. On the FAILED
/// terminal state the §5.3 error body is embedded; otherwise error is null.
nlohmann::json BuildProgressResponse(const ImportTaskProgress& task);

}  // namespace cortrix::import
