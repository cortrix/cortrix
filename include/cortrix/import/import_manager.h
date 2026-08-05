#pragma once
#include <memory>
#include <optional>
#include <string>

#include "cortrix/auth/auth_context.h"
#include "cortrix/common/result.h"
#include "cortrix/import/connection_manager.h"
#include "cortrix/import/import_task_queue.h"
#include "cortrix/import/import_types.h"
#include "cortrix/import/query_executor.h"
#include "cortrix/import/spc_feed.h"
#include "cortrix/import/text_serializer.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/observability/trace_context.h"

namespace cortrix::import {

struct ImportManagerConfig {
    QueryConstraints query_constraints;
    FilterDslConstraints dsl_constraints;
    int default_merge_size = 10;        // §4.4 text.default_merge_size
    std::string pg_host;                 // metadata for the Block source URI (D3.5 resolves real)
    int pg_port = 5432;
    std::string pg_db = "postgres";
};

/// Database-import main orchestrator. Wires the pieces together: resolve
/// connection_ref → DSN, D2 fetch rows under the 5 security constraints, D4
/// textualize, D3 clear this table's prior Blocks, D5 feed the SPC pipeline, D6 run
/// it as an async task with progress/cancel, writing the operation_log around
/// it (S6).
///
/// Standalone composition (the constructor takes interfaces): in-memory secret /
/// connection store, the validating QueryExecutor (real PG → D3.5), InMemorySpcFeeder
/// + InMemoryBlockCleaner (real SPCPipeline / store DELETE → D3.5), InMemory task
/// store, and the real IOperationLogger (CE, frozen). Real server routing → D3.5.
class ImportManager {
public:
    ImportManager(std::shared_ptr<IConnectionManager> conn_mgr,
                  std::shared_ptr<IQueryExecutor> query_exec,
                  std::shared_ptr<TextSerializer> serializer,
                  std::shared_ptr<ISpcFeeder> spc_feeder,
                  std::shared_ptr<IBlockCleaner> block_cleaner,
                  std::shared_ptr<IImportTaskStore> task_store,
                  std::shared_ptr<observability::IOperationLogger> op_logger,
                  ImportManagerConfig config = {});

    /// D6 async entry (§3.2 start_import). Validates the request (D2 security gate +
    /// resolves the ref under the D7 tenant guard), estimates rows (R5 pre-check),
    /// writes operation_log `database_import` (S6), and enqueues the task. Returns the
    /// task_id (or a CX_ERR_F16A_* Status on a synchronous rejection — bad SQL /
    /// cross-tenant / auth). The `ctx` is reserved for the TraceContext (D3.5 wiring).
    Result<ImportTaskId> StartImport(const ImportRequest& req,
                                     const AuthContext& auth_ctx,
                                     const observability::TraceContext* ctx = nullptr);

    /// D6 progress query (§5.2). nullopt for an unknown task_id.
    std::optional<ImportTaskProgress> GetProgress(const ImportTaskId& task_id);

    /// D6 cancel (§3.6). false only for an unknown task_id.
    bool Cancel(const ImportTaskId& task_id);

private:
    /// The per-task unit of work handed to the queue (runs on a worker thread):
    /// resolve DSN → fetch → textualize → D3 cleanup → feed SPC, reporting progress
    /// and honoring cooperative cancel. Returns a CX_ERR_F16A_* Status on failure.
    Status RunImport(const ImportRequest& req, const AuthContext& auth_ctx,
                     ImportTaskHandle& handle);

    std::shared_ptr<IConnectionManager> conn_mgr_;
    std::shared_ptr<IQueryExecutor> query_exec_;
    std::shared_ptr<TextSerializer> serializer_;
    std::shared_ptr<ISpcFeeder> spc_feeder_;
    std::shared_ptr<IBlockCleaner> block_cleaner_;
    std::shared_ptr<IImportTaskStore> task_store_;
    std::shared_ptr<observability::IOperationLogger> op_logger_;
    ImportManagerConfig config_;
    std::unique_ptr<ImportTaskQueue> queue_;
};

}  // namespace cortrix::import
