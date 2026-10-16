#include <cstdint>
#include "cortrix/server/routes/enrich_routes.h"

#include <chrono>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "cortrix/logging/logging.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/http_server.h"
#include "cortrix/spc_enricher/enrich_retry_sweeper.h"
#include "cortrix/spc_enricher/enrich_state_store.h"

namespace cortrix {

namespace {

int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void RegisterEnrichRoutes(httplib::Server& server,
                          resource::INamespacePool& pool,
                          ApiKeyAuth& auth,
                          spc::EnrichRetrySweeper* sweeper,
                          std::vector<std::string> chain_tokens,
                          observability::IOperationLogger* oplog) {
    server.Post(
        "/api/v1/namespaces/:name/enrich/backfill",
        WithAuth(auth, kPermAdmin,
            [&pool, sweeper, chain_tokens, oplog](
                const httplib::Request& req, httplib::Response& res,
                const RequestContext& rctx) {
                auto ns_it = req.path_params.find("name");
                const std::string ns_name =
                    ns_it != req.path_params.end() ? ns_it->second : "";
                if (ns_name.empty()) {
                    WriteJsonError(res, Status::InvalidArgument(
                                            "CX_ERR_NS_INVALID: empty namespace"),
                                   rctx.request_id);
                    return;
                }
                if (chain_tokens.empty()) {
                    // Nothing configured to backfill toward (no enricher LLM).
                    WriteJsonError(
                        res,
                        Status::InvalidArgument(
                            "CX_ERR_ENRICH_BACKFILL_NOT_CONFIGURED: enricher chain "
                            "is not configured (enricher_llm role absent)"),
                        rctx.request_id);
                    return;
                }

                bool audit = true;
                if (!req.body.empty()) {
                    auto body = nlohmann::json::parse(req.body, nullptr, false);
                    if (!body.is_discarded() && body.is_object() &&
                        body.contains("audit") && body["audit"].is_boolean()) {
                        audit = body["audit"].get<bool>();
                    }
                }

                resource::NamespaceFacade facade(pool, ns_name);
                Status acq = facade.Acquire();
                if (!acq.ok()) {
                    WriteJsonError(res, acq, rctx.request_id);
                    return;
                }
                sqlite3* db = facade.store().db_handle();
                if (!db) {
                    WriteJsonError(res,
                                   Status::Internal("CX_ERR_ENRICH_BACKFILL_STORE: "
                                                    "store db unavailable"),
                                   rctx.request_id);
                    return;
                }

                const int64_t now = NowUnix();
                const std::unordered_set<std::string> configured(
                    chain_tokens.begin(), chain_tokens.end());
                int synthesized = 0;
                int removed = 0;

                if (audit) {
                    // The audit reconciles both directions of the ingest's index
                    // rule. First remove contextual index entries the rule excludes
                    // (written by audits and backfills before the gates existed),
                    // then synthesize pending rows for legacy / silently-degraded
                    // chunks (shared with tests; rows already pending keep their
                    // backoff). Each removal waits on the index's own delete, so a
                    // large affected namespace makes its first audit slow; later
                    // audits remove nothing.
                    auto rec = spc::ReconcileExcludedContextualLabels(
                        db, [&facade](uint64_t label) {
                            return facade.vec_index().MarkDelete(label);
                        });
                    if (!rec.ok()) {
                        WriteJsonError(res, rec.status(), rctx.request_id);
                        return;
                    }
                    removed = rec.value();
                    auto synth = spc::SynthesizeEnrichAuditRows(db, configured, now);
                    if (!synth.ok()) {
                        WriteJsonError(res, synth.status(), rctx.request_id);
                        return;
                    }
                    synthesized = synth.value();
                }

                auto made_due = spc::MakeAllPendingDueNow(db, now);
                if (!made_due.ok()) {
                    CORTRIX_LOG_WARN("spc", "enrich backfill: make-due failed ns={}: {}",
                                     ns_name, made_due.status().message());
                }
                const int enqueued = sweeper ? sweeper->RunSweepNow(ns_name) : 0;

                auto counts = spc::CountEnrichStates(db);
                nlohmann::json body;
                body["namespace"] = ns_name;
                body["audited"] = audit;
                body["removed"] = removed;  // 0 when audit is false
                body["synthesized"] = synthesized;
                body["enqueued"] = enqueued;
                if (counts.ok()) {
                    body["counts"] = {
                        {"total", counts.value().total},
                        {"ok", counts.value().ok},
                        {"pending_retry", counts.value().pending_retry},
                        {"failed_permanent", counts.value().failed_permanent}};
                }
                if (oplog) {
                    observability::OperationLogEntry e;
                    e.user_id = rctx.auth.user_id;
                    e.action = "enrich_backfill";
                    e.resource_type = "namespace";
                    e.resource_id = ns_name;
                    e.namespace_id = ns_name;
                    e.summary = "removed=" + std::to_string(removed) +
                                " synthesized=" + std::to_string(synthesized) +
                                " enqueued=" + std::to_string(enqueued);
                    e.trace_id = rctx.request_id;
                    oplog->Log(e);
                }
                WriteJsonResponse(res, 200, body, rctx.request_id);
            }));
}

}  // namespace cortrix
