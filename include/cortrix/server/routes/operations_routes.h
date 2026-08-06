#pragma once

namespace httplib { class Server; }

namespace cortrix {

class ApiKeyAuth;

namespace observability { class IOperationLogger; }

/// Register the operation-log query route.
///
/// Endpoint:
///   GET /api/v1/operations  -- the caller's data-operation history ("what did I
///                              do"), filtered + paginated. Read-only (kPermRead).
///
/// Query params (filter dimensions): action, action_in (csv multi-select),
/// namespace_id, user_id (admin cross-user — default = the caller's own user_id),
/// resource_type, trace_id, from_timestamp, to_timestamp (Unix ms), limit
/// (default 50, cap 200), offset, sort_order (ASC|DESC, default DESC).
///
/// Permission: by default a caller only sees their own rows; specifying a
/// `user_id` other than their own requires an admin key (kPermAdmin) or the
/// response is CX_ERR_OPLOG_UNAUTHORIZED. The 6 CX_ERR_OPLOG_* error identities
/// are returned as the GEN-Agent 4-field body (code / message / retryable /
/// category / retry_after_ms / structured_data).
///
/// `logger` is the DI-provided IOperationLogger over cortrix_global.db (the same
/// instance the Engine instrumentation sites write through). Open-Core: the route depends on
/// the public interface only; an embedder injects an extended logger transparently.
void RegisterOperationsRoutes(httplib::Server& server,
                              observability::IOperationLogger& logger,
                              ApiKeyAuth& auth);

}  // namespace cortrix
