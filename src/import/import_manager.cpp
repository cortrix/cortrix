#include <cstdint>
#include "cortrix/import/import_manager.h"

#include <ctime>

#include "cortrix/id/ulid.h"
#include "cortrix/import/import_error.h"
#include "cortrix/import/import_metrics.h"
#include "cortrix/import/import_response.h"
#include "cortrix/observability/operation_log_emitter.h"

namespace cortrix::import {

namespace {

// "import_<ulid>" task id.
ImportTaskId NewTaskId() { return "import_" + cortrix::id::GenerateUlid(); }

// A short, stable signature of the query for Block provenance + the overwrite
// match (source_query_signature). table+filter → "table:<keys>"; sql → its hash
// is overkill for V1 — use a truncated form so the signature stays human-skimmable.
std::string QuerySignature(const QueryRequest& q) {
    if (q.table) {
        std::string sig = *q.table + ":";
        if (q.filter.is_object() && !q.filter.empty()) {
            bool first = true;
            for (auto it = q.filter.begin(); it != q.filter.end(); ++it) {
                if (!first) sig += ",";
                sig += it.key();
                first = false;
            }
        } else {
            sig += "*";
        }
        return sig;
    }
    if (q.sql) {
        const std::string& s = *q.sql;
        return "sql:" + (s.size() > 64 ? s.substr(0, 64) : s);
    }
    return "";
}

std::string NowIso8601() {
    return ToIso8601Utc(std::chrono::system_clock::now());
}

}  // namespace

ImportManager::ImportManager(std::shared_ptr<IConnectionManager> conn_mgr,
                             std::shared_ptr<IQueryExecutor> query_exec,
                             std::shared_ptr<TextSerializer> serializer,
                             std::shared_ptr<ISpcFeeder> spc_feeder,
                             std::shared_ptr<IBlockCleaner> block_cleaner,
                             std::shared_ptr<IImportTaskStore> task_store,
                             std::shared_ptr<observability::IOperationLogger> op_logger,
                             ImportManagerConfig config)
    : conn_mgr_(std::move(conn_mgr)),
      query_exec_(std::move(query_exec)),
      serializer_(std::move(serializer)),
      spc_feeder_(std::move(spc_feeder)),
      block_cleaner_(std::move(block_cleaner)),
      task_store_(std::move(task_store)),
      op_logger_(std::move(op_logger)),
      config_(std::move(config)),
      queue_(std::make_unique<ImportTaskQueue>(task_store_, ImportTaskQueueConfig{})) {}

Result<ImportTaskId> ImportManager::StartImport(const ImportRequest& req,
                                                const AuthContext& auth_ctx,
                                                const observability::TraceContext* /*ctx*/) {
    // permission: per-NS admin only (require_per_ns_admin). A non-admin caller
    // is rejected synchronously.
    if (!auth_ctx.is_admin()) {
        return ImportStatus(ImportErrorCode::kAuthDenied,
                          "per-NS admin is required to start a database import");
    }

    // +: resolve the connection_ref under the tenant guard (this is the
    // synchronous security gate — a cross-tenant ref throws ImportException; an expired
    // / revoked / missing ref returns AUTH_DENIED). We resolve once here to fail fast
    // (and to learn the tenant for the worker); the DSN itself is re-resolved in the
    // worker so it is never held across the queue boundary.
    Result<std::string> dsn = conn_mgr_->ResolveDsn(req.connection_ref, auth_ctx);
    if (!dsn.ok()) return dsn.status();

    // security gate (pre-validate before we ever enqueue) — bad SQL / DSL fails
    // synchronously with CX_ERR_IMPORT_INVALID_SQL.
    Result<CompiledQuery> compiled =
        CompileQueryRequest(req.query, config_.query_constraints, config_.dsl_constraints);
    if (!compiled.ok()) return compiled.status();

    // R5 pre-check: COUNT(*) estimate (real PG → integration; standalone returns a transient
    // we treat as "unknown total", NOT a hard failure — the import still queues).
    int rows_total = 0;
    Result<int64_t> est = query_exec_->EstimateRowCount(*dsn, req.query, config_.query_constraints);
    if (est.ok()) {
        if (est.value() > config_.query_constraints.max_rows) {
            return ImportStatus(ImportErrorCode::kRowsLimitExceeded,
                              "estimated rows exceed the 10M cap");
        }
        rows_total = static_cast<int>(est.value());
    }

    // S6: write operation_log `database_import` (GEN-OperationLog, via the
    // frozen emitter helper). We set user_id / namespace from the auth context (the
    // instrumentation site supplies these; the thread-local default is "anonymous").
    if (op_logger_) {
        auto entry = observability::MakeEngineEntry(
            observability::EmitSite::kSpcPipeline, "database_import", req.namespace_id,
            /*resource_id=*/std::nullopt,
            "DB import: " + QuerySignature(req.query));
        entry.user_id = auth_ctx.user_id.empty() ? "anonymous" : auth_ctx.user_id;
        op_logger_->Log(entry);
    }

    ImportTaskId task_id = NewTaskId();
    ImportMetrics::Instance().SetQueueDepth(queue_->QueueDepth() + 1);
    // The work closure captures the request + the caller's auth context by value, so
    // each task re-resolves its ref under its own tenant guard on the worker thread
    // (no shared mutable state across concurrent submits).
    AuthContext ctx_copy = auth_ctx;
    return queue_->Submit(
        task_id, req.namespace_id,
        [this, req, ctx_copy](ImportTaskHandle& h) { return RunImport(req, ctx_copy, h); },
        rows_total);
}

Status ImportManager::RunImport(const ImportRequest& req, const AuthContext& auth_ctx,
                                ImportTaskHandle& handle) {
    // Re-resolve the DSN inside the worker (never held across the queue boundary, R2).
    Result<std::string> dsn = conn_mgr_->ResolveDsn(req.connection_ref, auth_ctx);
    if (!dsn.ok()) {
        ImportMetrics::Instance().RecordImport(ImportMetrics::ImportOutcome::kFailed);
        return dsn.status();
    }
    if (handle.CancelRequested()) return Status::Ok();  // cancelled before work

    //: fetch rows under the 5 security constraints (real PG → integration; standalone the
    // executor returns CONNECTION_FAILED after validating).
    Result<std::vector<DbRow>> rows =
        query_exec_->Execute(*dsn, req.query, config_.query_constraints);
    if (!rows.ok()) {
        ImportMetrics::Instance().RecordImport(ImportMetrics::ImportOutcome::kFailed);
        return rows.status();
    }
    if (handle.CancelRequested()) return Status::Ok();

    //: textualize (per_row / merge — no template).
    SourceContext src;
    src.host = config_.pg_host;
    src.port = config_.pg_port;
    src.db_name = config_.pg_db;
    src.table = req.query.table.value_or("custom_sql");
    src.connection_ref = req.connection_ref;
    src.import_task_id = handle.id();
    src.query_signature = QuerySignature(req.query);
    src.imported_by = auth_ctx.user_id;
    src.imported_at_iso = NowIso8601();
    auto chunks = serializer_->Serialize(rows.value(), req.text_strategy, src,
                                         req.merge_size > 0 ? req.merge_size
                                                            : config_.default_merge_size);

    //: clear this table's prior Blocks (ARCH — only this source prefix).
    Result<int> cleaned = block_cleaner_->CleanupSourceBlocks(req.namespace_id, SourcePrefix(src));
    if (!cleaned.ok()) {
        ImportMetrics::Instance().RecordImport(ImportMetrics::ImportOutcome::kFailed);
        return cleaned.status();
    }
    if (handle.CancelRequested()) return Status::Ok();

    //: feed the SPC pipeline (real SPCPipeline → integration; standalone records).
    Result<int> fed = spc_feeder_->Feed(chunks, req.namespace_id);
    if (!fed.ok()) {
        ImportMetrics::Instance().RecordImport(ImportMetrics::ImportOutcome::kFailed);
        return fed.status();
    }
    handle.ReportProgress(fed.value(), static_cast<int>(chunks.size()));

    ImportMetrics::Instance().RecordImport(ImportMetrics::ImportOutcome::kSuccess);
    ImportMetrics::Instance().AddRowsImported(fed.value());
    return Status::Ok();
}

std::optional<ImportTaskProgress> ImportManager::GetProgress(const ImportTaskId& task_id) {
    return queue_->GetProgress(task_id);
}

bool ImportManager::Cancel(const ImportTaskId& task_id) {
    bool ok = queue_->Cancel(task_id);
    if (ok) ImportMetrics::Instance().RecordImport(ImportMetrics::ImportOutcome::kCancelled);
    return ok;
}

}  // namespace cortrix::import
