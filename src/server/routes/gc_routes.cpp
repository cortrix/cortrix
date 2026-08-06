#include "cortrix/server/routes/gc_routes.h"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/catalog/gc/document_gc_sweeper.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/server/http_server.h"

namespace cortrix {

namespace {

using catalog::gc::DocumentGcSweeper;
using catalog::gc::GcManager;

// GEN-Agent 4-field error body (CLAUDE.md). Used for the local route-level
// checks (X-Ops-Confirm) that don't go through WriteJsonError's Status mapping.
void WriteAgentError(httplib::Response& res, int http_status, const std::string& code,
                     const std::string& message, agent_friendly::ErrorCategory category,
                     const std::string& request_id) {
    agent_friendly::AgentFriendlyError err;
    err.code = code;
    err.message = message;
    err.retryable = false;
    err.category = category;
    nlohmann::json body;
    body["error"] = agent_friendly::ToJson(err);
    if (!request_id.empty()) {
        body["error"]["request_id"] = request_id;
        res.set_header("X-Request-Id", request_id);
    }
    res.status = http_status;
    res.set_content(body.dump(), "application/json");
}

// ops.yaml: run / purge require `X-Ops-Confirm: true`. Returns false + writes the
// 400 error body when the header is missing/not "true".
bool RequireOpsConfirm(const httplib::Request& req, httplib::Response& res,
                       const std::string& request_id) {
    auto it = req.headers.find("X-Ops-Confirm");
    if (it == req.headers.end() || it->second != "true") {
        WriteAgentError(res, 400, "CX_ERR_OPS_CONFIRM_REQUIRED",
                        "this destructive op requires the X-Ops-Confirm: true header",
                        agent_friendly::ErrorCategory::kPermanent, request_id);
        return false;
    }
    return true;
}

// Build the GcStatus wire body (schemas.yaml: status / soft_deleted_count /
// reclaimable_bytes / last_gc_at). soft_deleted_count comes from the document
// layer (the sweeper); reclaimable_bytes / last_gc_at from the catalog layer
// (GcManager). `running` reflects the catalog manager's in-flight sweep.
void WriteGcStatus(httplib::Response& res, GcManager& gc, DocumentGcSweeper& sweeper,
                   int http_status, const std::string& request_id) {
    auto cat = gc.GetStatus();
    if (!cat.ok()) {
        WriteJsonError(res, cat.status(), request_id);
        return;
    }
    auto soft = sweeper.CountSoftDeleted();
    if (!soft.ok()) {
        WriteJsonError(res, soft.status(), request_id);
        return;
    }
    const auto& c = cat.value();
    nlohmann::json j;
    j["status"] = c.running ? "running" : "idle";
    j["soft_deleted_count"] = soft.value();
    j["reclaimable_bytes"] = c.reclaimable_bytes;
    // Integer Unix-epoch ms per the project timestamp convention (spec S2-C ruling).
    j["last_gc_at"] = c.last_gc_at_ms.has_value()
                          ? nlohmann::json(*c.last_gc_at_ms)
                          : nlohmann::json(nullptr);
    WriteJsonResponse(res, http_status, j, request_id);
}

}  // namespace

void RegisterGcRoutes(httplib::Server& server, GcManager& gc,
                      DocumentGcSweeper& sweeper, ApiKeyAuth& auth) {
    // GET /api/v1/gc/status — read-only snapshot (ops:admin).
    server.Get("/api/v1/gc/status",
        WithAuth(auth, kPermAdmin,
            [&gc, &sweeper](const httplib::Request&, httplib::Response& res,
                            const RequestContext& rctx) {
                WriteGcStatus(res, gc, sweeper, 200, rctx.request_id);
            }));

    // POST /api/v1/gc/run — trigger one sweep over both layers (X-Ops-Confirm).
    server.Post("/api/v1/gc/run",
        WithAuth(auth, kPermAdmin,
            [&gc, &sweeper](const httplib::Request& req, httplib::Response& res,
                            const RequestContext& rctx) {
                if (!RequireOpsConfirm(req, res, rctx.request_id)) return;
                // Catalog layer (blob_gc_queue) then document layer (per-Unit).
                auto r = gc.RunOnce();
                if (!r.ok()) {
                    WriteJsonError(res, r.status(), rctx.request_id);
                    return;
                }
                auto d = sweeper.RunOnce();
                if (!d.ok()) {
                    WriteJsonError(res, d.status(), rctx.request_id);
                    return;
                }
                WriteGcStatus(res, gc, sweeper, 202, rctx.request_id);
            }));

    // POST /api/v1/gc/restore — restore soft-deleted documents (PartialSuccessById).
    server.Post("/api/v1/gc/restore",
        WithAuth(auth, kPermAdmin,
            [&sweeper](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
                std::vector<std::string> doc_ids;
                try {
                    auto body = nlohmann::json::parse(req.body);
                    if (body.contains("document_ids") && body["document_ids"].is_array()) {
                        for (const auto& d : body["document_ids"]) {
                            if (d.is_string()) doc_ids.push_back(d.get<std::string>());
                        }
                    }
                } catch (const std::exception&) {
                    WriteAgentError(res, 400, "CX_ERR_GC_INVALID_BODY",
                                    "request body must be JSON {document_ids: [...]}",
                                    agent_friendly::ErrorCategory::kPermanent, rctx.request_id);
                    return;
                }

                auto r = sweeper.Restore(doc_ids);
                if (!r.ok()) {
                    WriteJsonError(res, r.status(), rctx.request_id);
                    return;
                }
                const auto& out = r.value();

                // PartialSuccessByIdV1: succeeded[] objects with id, failed[]
                // objects with the GEN-Agent error fields, + coverage_ratio.
                nlohmann::json succeeded = nlohmann::json::array();
                for (const auto& id : out.succeeded) succeeded.push_back({{"id", id}});
                nlohmann::json failed = nlohmann::json::array();
                for (const auto& [id, code] : out.failed) {
                    failed.push_back({{"id", id},
                                      {"error_code", code},
                                      {"retryable", false},
                                      {"category", "permanent"}});
                }
                const size_t total = out.succeeded.size() + out.failed.size();
                const double coverage =
                    total == 0 ? 1.0 : static_cast<double>(out.succeeded.size()) / total;

                nlohmann::json body;
                body["succeeded"] = std::move(succeeded);
                body["failed"] = std::move(failed);
                body["coverage_ratio"] = coverage;
                WriteJsonResponse(res, 200, body, rctx.request_id);
            }));

    // POST /api/v1/gc/purge — immediate purge, bypasses windows (X-Ops-Confirm).
    server.Post("/api/v1/gc/purge",
        WithAuth(auth, kPermAdmin,
            [&gc, &sweeper](const httplib::Request& req, httplib::Response& res,
                            const RequestContext& rctx) {
                if (!RequireOpsConfirm(req, res, rctx.request_id)) return;
                auto r = gc.Purge();
                if (!r.ok()) {
                    WriteJsonError(res, r.status(), rctx.request_id);
                    return;
                }
                auto d = sweeper.Purge();
                if (!d.ok()) {
                    WriteJsonError(res, d.status(), rctx.request_id);
                    return;
                }
                WriteGcStatus(res, gc, sweeper, 202, rctx.request_id);
            }));

    // POST /api/v1/maintenance/vacuum — reclaim + compact catalog.db.
    server.Post("/api/v1/maintenance/vacuum",
        WithAuth(auth, kPermAdmin,
            [&gc](const httplib::Request&, httplib::Response& res,
                  const RequestContext& rctx) {
                Status s = gc.Vacuum();
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 202, {{"status", "accepted"}, {"op", "vacuum"}},
                                  rctx.request_id);
            }));

    // POST /api/v1/maintenance/reindex — rebuild catalog.db indexes.
    server.Post("/api/v1/maintenance/reindex",
        WithAuth(auth, kPermAdmin,
            [&gc](const httplib::Request&, httplib::Response& res,
                  const RequestContext& rctx) {
                Status s = gc.Reindex();
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 202, {{"status", "accepted"}, {"op", "reindex"}},
                                  rctx.request_id);
            }));
}

}  // namespace cortrix
