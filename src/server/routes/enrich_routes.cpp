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

                // The body is optional; when present it must be a JSON object whose
                // keys are the two boolean flags. Anything else is rejected with a
                // named error rather than ignored: a misspelled key or a string
                // "true" used to get a 200 that did nothing, and that nothing
                // looked exactly like "nothing to do".
                bool audit = true;
                bool reset_given_up = false;
                if (!req.body.empty()) {
                    auto body = nlohmann::json::parse(req.body, nullptr, false);
                    auto bad = [&](const std::string& what) {
                        WriteJsonError(res,
                                       Status::InvalidArgument(
                                           "CX_ERR_ENRICH_BACKFILL_INVALID_BODY: " + what),
                                       rctx.request_id);
                    };
                    if (body.is_discarded() || !body.is_object()) {
                        bad("request body must be a JSON object with the optional boolean "
                            "fields audit and reset_given_up");
                        return;
                    }
                    for (auto it = body.begin(); it != body.end(); ++it) {
                        if (it.key() != "audit" && it.key() != "reset_given_up") {
                            bad("unknown field '" + it.key() + "'");
                            return;
                        }
                        if (!it.value().is_boolean()) {
                            bad("field '" + it.key() + "' must be a boolean");
                            return;
                        }
                    }
                    if (body.contains("audit")) audit = body["audit"].get<bool>();
                    if (body.contains("reset_given_up")) {
                        reset_given_up = body["reset_given_up"].get<bool>();
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
                int settled = 0;
                int reset = 0;
                int orphaned = 0;

                // A failure once the audit has started answers the error with
                // structured_data.removed, the entries this request removed before it:
                // those removals stay in place. When there were any, they are logged
                // first, as the success path logs them. The log entry and the response
                // take the code from the same resolver.
                auto audit_failure = [&](const Status& s, int removed_so_far) {
                    if (oplog && removed_so_far > 0) {
                        observability::OperationLogEntry e;
                        e.user_id = rctx.auth.user_id;
                        e.action = "enrich_backfill";
                        e.resource_type = "namespace";
                        e.resource_id = ns_name;
                        e.namespace_id = ns_name;
                        e.summary = "removed=" + std::to_string(removed_so_far) +
                                    " failed=" + ResolvedErrorCode(s);
                        e.trace_id = rctx.request_id;
                        oplog->Log(e);
                    }
                    WriteJsonErrorWithData(res, s, nlohmann::json{{"removed", removed_so_far}},
                                           rctx.request_id);
                };

                if (audit) {
                    // The audit reconciles both directions of the ingest's index
                    // rule. First remove contextual index entries the rule excludes
                    // (written by audits and backfills before the gates existed),
                    // then synthesize pending rows for legacy / silently-degraded
                    // chunks (shared with tests; rows already pending keep their
                    // backoff) and settle terminal rows whose artifacts are all
                    // present. Each removal waits on the index's own delete, so a
                    // large affected namespace makes its first audit slow; later
                    // audits remove nothing.
                    auto rec = spc::ReconcileExcludedContextualLabels(
                        db, [&facade](uint64_t label) {
                            return facade.vec_index().MarkDelete(label);
                        });
                    if (!rec.status.ok()) {
                        audit_failure(rec.status, rec.removed);
                        return;
                    }
                    removed = rec.removed;
                    auto synth = spc::SynthesizeEnrichAuditRows(db, configured, now, &settled);
                    if (!synth.ok()) {
                        audit_failure(synth.status(), removed);
                        return;
                    }
                    synthesized = synth.value();
                }

                if (reset_given_up) {
                    // The operator's explicit exit for rows that gave up: back to
                    // pending with a fresh count, due now; rows whose chunk is gone
                    // are left terminal and reported, not moved. Runs after the audit
                    // so a row the audit just settled is not re-delivered.
                    auto rr = spc::ResetGivenUpEnrichRows(db, now);
                    if (!rr.ok()) {
                        audit_failure(rr.status(), removed);  // 0 when the audit was off
                        return;
                    }
                    reset = rr.value().moved;
                    orphaned = rr.value().orphaned;
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
                body["reset_given_up"] = reset_given_up;
                body["synthesized"] = synthesized;
                body["settled"] = settled;            // 0 when audit is false
                body["reset"] = reset;                // 0 when reset_given_up is false
                body["orphaned_skipped"] = orphaned;  // terminal rows whose chunk is gone
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
                                " settled=" + std::to_string(settled) +
                                " reset=" + std::to_string(reset) +
                                " orphaned=" + std::to_string(orphaned) +
                                " enqueued=" + std::to_string(enqueued);
                    e.trace_id = rctx.request_id;
                    oplog->Log(e);
                }
                WriteJsonResponse(res, 200, body, rctx.request_id);
            }));
}

}  // namespace cortrix
