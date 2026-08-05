#include "cortrix/server/import_handler.h"

#include "cortrix/agent_friendly/error.h"
#include "cortrix/import/import_error.h"
#include "cortrix/import/import_response.h"

namespace cortrix::server {

using cortrix::import::F16aErrorCode;
using cortrix::import::GetF16aErrorInfo;
using cortrix::import::ImportRequest;
using cortrix::import::MakeF16aError;
using cortrix::import::ParseTextStrategy;
using cortrix::import::TextStrategy;

namespace {

// Recover the F16aErrorCode whose CX_ERR_IMPORT_* token a Status message is prefixed
// with (F16aStatus / F16aException stamp "CX_ERR_IMPORT_X: detail"). Falls back to
// kConnectionFailed for an unrecognized prefix (a transient 503 is the safest
// default for an internal failure the Agent may retry).
F16aErrorCode CodeFromStatus(const cortrix::Status& s) {
    const std::string& m = s.message();
    for (F16aErrorCode c : {F16aErrorCode::kConnectionFailed, F16aErrorCode::kAuthDenied,
                            F16aErrorCode::kInvalidSql, F16aErrorCode::kTimeout,
                            F16aErrorCode::kRowsLimitExceeded, F16aErrorCode::kCrossTenantRef}) {
        if (m.rfind(GetF16aErrorInfo(c).cx_code, 0) == 0) return c;
    }
    return F16aErrorCode::kConnectionFailed;
}

// Build the §5.3 Agent-friendly error envelope { "error": {...} } from a Status.
nlohmann::json ErrorBody(const cortrix::Status& s) {
    F16aErrorCode code = CodeFromStatus(s);
    auto err = MakeF16aError(code, nlohmann::json::object(), s.message());
    nlohmann::json body;
    body["error"] = cortrix::agent_friendly::ToJson(err);
    return body;
}

int HttpFor(const cortrix::Status& s) { return GetF16aErrorInfo(CodeFromStatus(s)).http_status; }

}  // namespace

ImportHandler::ImportHandler(std::shared_ptr<cortrix::import::ImportManager> import_mgr,
                             std::shared_ptr<cortrix::import::IConnectionManager> conn_mgr)
    : import_mgr_(std::move(import_mgr)), conn_mgr_(std::move(conn_mgr)) {}

cortrix::Result<ImportRequest> ImportHandler::ParseImportRequest(const nlohmann::json& body) {
    using cortrix::import::F16aStatus;
    if (!body.is_object()) {
        return F16aStatus(F16aErrorCode::kInvalidSql, "request body must be a JSON object");
    }
    ImportRequest req;
    if (!body.contains("namespace") || !body["namespace"].is_string()) {
        return F16aStatus(F16aErrorCode::kInvalidSql, "namespace is required");
    }
    req.namespace_id = body["namespace"].get<std::string>();
    if (!body.contains("connection_ref") || !body["connection_ref"].is_string()) {
        return F16aStatus(F16aErrorCode::kInvalidSql, "connection_ref is required");
    }
    req.connection_ref = body["connection_ref"].get<std::string>();

    // text_strategy (default per_row; "template" rejected — v1.0.2 resolution 13).
    if (body.contains("text_strategy")) {
        if (!body["text_strategy"].is_string()) {
            return F16aStatus(F16aErrorCode::kInvalidSql, "text_strategy must be a string");
        }
        auto ts = ParseTextStrategy(body["text_strategy"].get<std::string>());
        if (!ts) {
            return F16aStatus(F16aErrorCode::kInvalidSql,
                              "unsupported text_strategy (per_row|merge only)");
        }
        req.text_strategy = *ts;
    }
    if (body.contains("merge_size") && body["merge_size"].is_number_integer()) {
        req.merge_size = body["merge_size"].get<int>();
    }

    // oneOf{table, sql}.
    const bool has_table = body.contains("table") && body["table"].is_string();
    const bool has_sql = body.contains("sql") && body["sql"].is_string();
    if (has_table == has_sql) {
        return F16aStatus(F16aErrorCode::kInvalidSql,
                          "exactly one of {table, sql} must be provided");
    }
    if (has_table) {
        req.query.table = body["table"].get<std::string>();
        if (body.contains("filter")) req.query.filter = body["filter"];  // validated downstream
        if (body.contains("columns") && body["columns"].is_array()) {
            for (const auto& c : body["columns"]) {
                if (c.is_string()) req.query.columns.push_back(c.get<std::string>());
            }
        }
    } else {
        req.query.sql = body["sql"].get<std::string>();
    }
    return req;
}

nlohmann::json ImportHandler::HandleStartImport(const nlohmann::json& body,
                                                const AuthContext& auth_ctx,
                                                int& out_http_status) {
    auto parsed = ParseImportRequest(body);
    if (!parsed.ok()) {
        out_http_status = HttpFor(parsed.status());
        return ErrorBody(parsed.status());
    }
    try {
        auto r = import_mgr_->StartImport(parsed.value(), auth_ctx);
        if (!r.ok()) {
            out_http_status = HttpFor(r.status());
            return ErrorBody(r.status());
        }
        auto progress = import_mgr_->GetProgress(r.value());
        out_http_status = 200;
        if (progress) {
            // estimated_rows = rows_total seeded from the COUNT(*) pre-check (0 if unknown).
            return cortrix::import::BuildImportStartedResponse(
                *progress, parsed.value().connection_ref, progress->rows_total);
        }
        // task vanished immediately (shouldn't happen) — minimal queued body.
        cortrix::import::ImportTaskProgress p;
        p.task_id = r.value();
        p.namespace_id = parsed.value().namespace_id;
        p.queued_at = std::chrono::system_clock::now();
        return cortrix::import::BuildImportStartedResponse(p, parsed.value().connection_ref, 0);
    } catch (const cortrix::import::F16aException& e) {
        // a cross-tenant ref throws (anti-enumeration path).
        out_http_status = GetF16aErrorInfo(e.code()).http_status;
        nlohmann::json out;
        out["error"] = cortrix::agent_friendly::ToJson(e.GetError());
        return out;
    } catch (const std::exception& e) {
        // (R2-M5) Any other throw from the import stack (DB driver, std::bad_alloc, ...)
        // returns a structured Agent-friendly 500 here, instead of escaping to the
        // generic global exception handler. CX_ERR_IMPORT_INTERNAL = transient/retryable.
        out_http_status =
            GetF16aErrorInfo(cortrix::import::F16aErrorCode::kInternal).http_status;
        nlohmann::json out;
        out["error"] = cortrix::agent_friendly::ToJson(cortrix::import::MakeF16aError(
            cortrix::import::F16aErrorCode::kInternal, nlohmann::json::object(),
            std::string("import failed: ") + e.what()));
        return out;
    }
}

nlohmann::json ImportHandler::HandleGetProgress(const std::string& task_id,
                                                int& out_http_status) {
    auto p = import_mgr_->GetProgress(task_id);
    if (!p) {
        out_http_status = 404;
        nlohmann::json out;
        out["error"] = {{"code", "CX_ERR_IMPORT_TASK_NOT_FOUND"},
                        {"message", "import task not found: " + task_id},
                        {"retryable", false},
                        {"category", "permanent"},
                        {"retry_after_ms", nullptr},
                        {"structured_data", {{"task_id", task_id}}}};
        return out;
    }
    out_http_status = 200;
    return cortrix::import::BuildProgressResponse(*p);
}

nlohmann::json ImportHandler::HandleCancel(const std::string& task_id, int& out_http_status) {
    if (!import_mgr_->Cancel(task_id)) {
        out_http_status = 404;
        nlohmann::json out;
        out["error"] = {{"code", "CX_ERR_IMPORT_TASK_NOT_FOUND"},
                        {"message", "import task not found: " + task_id},
                        {"retryable", false},
                        {"category", "permanent"},
                        {"retry_after_ms", nullptr},
                        {"structured_data", {{"task_id", task_id}}}};
        return out;
    }
    auto p = import_mgr_->GetProgress(task_id);
    out_http_status = 200;
    nlohmann::json out;
    out["task_id"] = task_id;
    out["status"] = p ? cortrix::import::ToString(p->status) : "cancelling";
    return out;
}

nlohmann::json ImportHandler::HandleRegisterConnection(const nlohmann::json& body,
                                                       const AuthContext& auth_ctx,
                                                       int& out_http_status) {
    using cortrix::import::F16aStatus;
    if (!auth_ctx.is_admin()) {
        auto s = F16aStatus(F16aErrorCode::kAuthDenied, "admin required to register a connection");
        out_http_status = HttpFor(s);
        return ErrorBody(s);
    }
    if (!body.is_object() || !body.contains("name") || !body.contains("dsn")) {
        auto s = F16aStatus(F16aErrorCode::kInvalidSql, "name and dsn are required");
        out_http_status = HttpFor(s);
        return ErrorBody(s);
    }
    std::optional<int> expire_days;
    if (body.contains("expire_days") && body["expire_days"].is_number_integer()) {
        expire_days = body["expire_days"].get<int>();
    }
    auto r = conn_mgr_->Register(body["name"].get<std::string>(), body["dsn"].get<std::string>(),
                                 auth_ctx.tenant_id, auth_ctx.user_id, expire_days);
    if (!r.ok()) {
        out_http_status = HttpFor(r.status());
        return ErrorBody(r.status());
    }
    out_http_status = 200;
    // Return only the ref + metadata, never the DSN (R2).
    nlohmann::json out;
    out["ref_id"] = r.value();
    out["name"] = body["name"];
    return out;
}

nlohmann::json ImportHandler::HandleListConnections(const AuthContext& auth_ctx,
                                                    int& out_http_status) {
    using cortrix::import::F16aStatus;
    if (!auth_ctx.is_admin()) {
        auto s = F16aStatus(F16aErrorCode::kAuthDenied, "admin required to list connections");
        out_http_status = HttpFor(s);
        return ErrorBody(s);
    }
    out_http_status = 200;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& c : conn_mgr_->ListConnections(auth_ctx.tenant_id)) {
        arr.push_back({{"ref_id", c.ref_id},
                       {"name", c.name},
                       {"host", c.host},
                       {"db_name", c.db_name},
                       {"registered_by", c.registered_by},
                       {"revoked", c.revoked}});  // NO dsn (R2)
    }
    nlohmann::json out;
    out["connections"] = arr;
    return out;
}

nlohmann::json ImportHandler::HandleRevokeConnection(const std::string& ref_id,
                                                     const nlohmann::json& body,
                                                     const AuthContext& auth_ctx,
                                                     int& out_http_status) {
    std::string reason = (body.is_object() && body.contains("reason") && body["reason"].is_string())
                             ? body["reason"].get<std::string>()
                             : "";
    auto s = conn_mgr_->Revoke(ref_id, auth_ctx, reason);
    if (!s.ok()) {
        out_http_status = HttpFor(s);
        return ErrorBody(s);
    }
    out_http_status = 200;
    nlohmann::json out;
    out["ref_id"] = ref_id;
    out["revoked"] = true;
    return out;
}

}  // namespace cortrix::server
