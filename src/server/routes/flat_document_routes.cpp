#include <cstdint>
#include "cortrix/server/routes/flat_document_routes.h"

#include "cortrix/server/routes/connector_routes.h"  // ConnectorState (shared registry)
#include "cortrix/connector/dir_watcher_registry.h"  // fan-out registry (/watch)
#include "cortrix/catalog/i_ns_router.h"             // INSRouter (signature compat)
#include "cortrix/spc/spc_manager.h"                 // SPCManager (signature compat)
#include "cortrix/upload/upload_handler.h"
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/async/document_task_handler.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"
#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/json_depth.h"  // metadata depth guard (DoS: deep-JSON dump)
#include "cortrix/id/ulid.h"
#include "cortrix/logging/logging.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

namespace cortrix {

namespace {

// CortrixDoc internal DocStatus -> the openapi Document `status` enum
// (api/components/schemas.yaml#/Document.status:
//   pending|parsing|chunking|embedding|indexing|ready|failed). The live store
// tracks a coarser lifecycle (pending/processing/ready/error/stale/...), so map
// processing -> the spec's mid-pipeline placeholder and error -> failed. The SDK
// Document.status is a required literal of the spec enum, so the flat surface must
// emit one of these tokens (the nested /namespaces route's free-form string would
// fail SDK parsing — design surface keeps the spec contract).
std::string DocStatusToSpec(DocStatus status) {
    switch (status) {
        case DocStatus::kPending:    return "pending";
        case DocStatus::kProcessing: return "parsing";
        case DocStatus::kReady:      return "ready";
        case DocStatus::kError:      return "failed";
        case DocStatus::kStale:      return "pending";   // re-ingest pending
        default:                     return "failed";    // deleting/deleted not externally live
    }
}

// Serialize a CortrixDoc into the openapi Document object (document_id + status
// required; the rest optional). Field names follow the spec / SDK type
// (document_id, NOT the live store's doc_id), so SDK parse_model() binds the
// required `document_id` + `status`.
nlohmann::json DocToSpecJson(const CortrixDoc& doc, const std::string& namespace_id) {
    nlohmann::json d;
    d["document_id"] = doc.doc_id;
    d["namespace"] = namespace_id;
    d["filename"] = doc.source_path;
    d["source_type"] = doc.source_type;
    // source_ref carries the external origin locator when one exists (watch_dir data
    // dir, S3 URI for connector-ingested docs, …); empty for managed uploads whose
    // canonical location is the Cortrix store itself. Surfaced so consumers (SDK /
    // agent / UI) can answer "where did this come from / how do I find the source".
    d["source_ref"] = doc.source_ref;
    d["status"] = DocStatusToSpec(doc.status);
    d["content_hash"] = doc.content_hash;
    d["created_at"] = doc.created_at;
    return d;
}

// Resolve the required ?namespace= query param. On absence writes a 400 GEN-Agent
// envelope (structured_data.missing_param) and returns false. Per-NS storage model
// inference (F13 precedent): the spec GET/DELETE /documents/{id} carry no namespace,
// but live storage is per-namespace, so the flat surface requires it explicitly.
bool RequireNamespaceParam(const httplib::Request& req, httplib::Response& res,
                           const std::string& request_id, std::string* out_ns) {
    if (!req.has_param("namespace")) {
        agent_friendly::AgentFriendlyError err;
        err.code = "CX_ERR_MISSING_PARAM";
        err.message =
            "namespace query parameter is required (documents are namespace-scoped)";
        err.category = agent_friendly::ErrorCategory::kPermanent;
        err.retryable = false;
        err.structured_data = nlohmann::json{{"missing_param", "namespace"}};
        // R2-M8: wrapped envelope ("error" key) to match WriteJsonError + all
        // other error responses (the GET/DELETE flat surface stays consistent).
        nlohmann::json error_body;
        error_body["error"] = agent_friendly::ToJson(err);
        WriteJsonResponse(res, 400, error_body, request_id);
        return false;
    }
    *out_ns = req.get_param_value("namespace");
    if (out_ns->empty()) {
        agent_friendly::AgentFriendlyError err;
        err.code = "CX_ERR_MISSING_PARAM";
        err.message = "namespace query parameter must be non-empty";
        err.category = agent_friendly::ErrorCategory::kPermanent;
        err.retryable = false;
        err.structured_data = nlohmann::json{{"missing_param", "namespace"}};
        // R2-M8: wrapped envelope ("error" key) to match WriteJsonError + all
        // other error responses (the GET/DELETE flat surface stays consistent).
        nlohmann::json error_body;
        error_body["error"] = agent_friendly::ToJson(err);
        WriteJsonResponse(res, 400, error_body, request_id);
        return false;
    }
    return true;
}

}  // namespace

void RegisterFlatDocumentRoutes(httplib::Server& server,
                                resource::INamespacePool& pool,
                                UploadHandler& handler,
                                server::BatchSubmitService& batch_service,
                                async::DocumentTaskHandler& task_handler,
                                ApiKeyAuth& auth) {

    // POST /api/v1/documents — async upload (JSON body {namespace, content,
    // filename?, metadata?}; api/paths/documents.yaml uploadDocument) -> 202
    // DocumentTask. Reuses the BatchSubmitService as a batch-of-1: the service
    // already materializes inline content to a server-side file and fans it through
    // the frozen task scheduler, returning a task_id. The single-doc reply is reshaped
    // from the batch results[0] into the spec DocumentTask shape.
    server.Post("/api/v1/documents",
        WithAuth(auth, kPermWrite,
            [&batch_service](const httplib::Request& req, httplib::Response& res,
                             const RequestContext& rctx) {
                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch (...) {
                    WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"),
                                   rctx.request_id);
                    return;
                }
                if (!body.is_object()) {
                    WriteJsonError(res, Status::InvalidArgument("request body must be a JSON object"),
                                   rctx.request_id);
                    return;
                }
                if (!body.contains("namespace") || !body["namespace"].is_string()) {
                    WriteJsonError(res, Status::InvalidArgument("missing or invalid 'namespace'"),
                                   rctx.request_id);
                    return;
                }
                if (!body.contains("content") || !body["content"].is_string()) {
                    WriteJsonError(res, Status::InvalidArgument("missing or invalid 'content'"),
                                   rctx.request_id);
                    return;
                }
                const std::string namespace_id = body["namespace"].get<std::string>();

                // Fan a single document through the batch service. The doc_id keys
                // the materialized temp file + task dedup; a fresh ULID is unique per
                // request (the pipeline derives the real content-hash identity).
                server::BatchRequest batch;
                batch.namespace_id = namespace_id;
                server::BatchDocument doc;
                doc.doc_id = id::GenerateUlid();
                doc.content = body["content"].get<std::string>();
                // Optional client filename (the {filename?} in the route contract):
                // its extension drives server-side parser selection (".txt" fallback).
                if (body.contains("filename") && body["filename"].is_string()) {
                    doc.filename = body["filename"].get<std::string>();
                }
                if (body.contains("metadata") && body["metadata"].is_object()) {
                    // Reject over-deep metadata before dump() (json_depth.h): a deeply
                    // nested object would otherwise overflow the stack inside dump().
                    const int meta_depth = JsonMaxDepth(body["metadata"]);
                    if (meta_depth > kMaxMetadataDepth) {
                        nlohmann::json error_body;
                        error_body["error"] =
                            agent_friendly::ToJson(MakeMetadataTooDeepError(meta_depth));
                        WriteJsonResponse(res, 422, error_body, rctx.request_id);
                        return;
                    }
                    doc.metadata_json = body["metadata"].dump();
                }
                batch.documents.push_back(std::move(doc));

                server::BatchHttpResult br = batch_service.Submit(batch);

                // A batch-level rejection (e.g. payload too large) is already the
                // GEN-Agent envelope — surface it verbatim.
                if (br.status != 200) {
                    if (!rctx.request_id.empty()) res.set_header("X-Request-Id", rctx.request_id);
                    res.status = br.status;
                    res.set_content(br.body.dump(), "application/json");
                    return;
                }

                // partial-success body: a single doc either succeeded (results[0]
                // carries task_id) or failed (meta.failed[0]).
                const auto& results = br.body.value("results", nlohmann::json::array());
                if (!results.empty() && results[0].contains("task_id")) {
                    nlohmann::json task;
                    task["task_id"] = results[0]["task_id"];
                    task["namespace"] = namespace_id;
                    task["status"] = "queued";  // spec DocumentTask enum (fresh enqueue)
                    WriteJsonResponse(res, 202, task, rctx.request_id);
                    return;
                }
                // Per-doc failure → propagate the GEN-Agent failure descriptor as a 400.
                const auto& meta = br.body.value("meta", nlohmann::json::object());
                const auto& failed = meta.value("failed", nlohmann::json::array());
                nlohmann::json err = failed.empty()
                    ? nlohmann::json{{"code", "CX_ERR_INTERNAL_ERROR"},
                                     {"message", "upload submission failed"}}
                    : failed[0];
                WriteJsonResponse(res, 400, err, rctx.request_id);
            }
        )
    );

    // GET /api/v1/documents?namespace=&limit=&offset= — list (api/paths/documents.yaml
    // listDocuments; namespace required). Reuses the per-NS façade list logic of the
    // nested route, reshaped into the spec {documents:[Document], total} body.
    server.Get("/api/v1/documents",
        WithAuth(auth, kPermRead,
            [&pool](const httplib::Request& req, httplib::Response& res,
                    const RequestContext& rctx) {
                std::string ns_name;
                if (!RequireNamespaceParam(req, res, rctx.request_id, &ns_name)) return;

                resource::NamespaceFacade facade(pool, ns_name);
                Status acq = facade.Acquire();
                if (!acq.ok()) {
                    WriteJsonError(res, Status::NotFound(
                        "namespace not found: " + ns_name + ": " + acq.message()),
                        rctx.request_id);
                    return;
                }

                std::vector<CortrixDoc> all_docs;
                static const DocStatus kStatuses[] = {
                    DocStatus::kPending, DocStatus::kProcessing,
                    DocStatus::kReady, DocStatus::kError, DocStatus::kStale
                };
                for (auto status : kStatuses) {
                    std::vector<CortrixDoc> docs;
                    facade.store().doc_list_by_status(status, docs);
                    all_docs.insert(all_docs.end(), docs.begin(), docs.end());
                }
                std::sort(all_docs.begin(), all_docs.end(),
                    [](const CortrixDoc& a, const CortrixDoc& b) {
                        return a.doc_id > b.doc_id;
                    });

                int limit = 50;
                int offset = 0;
                if (req.has_param("limit")) {
                    try { limit = std::min(std::stoi(req.get_param_value("limit")), 200); } catch (...) {}
                }
                if (req.has_param("offset")) {
                    try { offset = std::max(std::stoi(req.get_param_value("offset")), 0); } catch (...) {}
                }

                nlohmann::json docs_array = nlohmann::json::array();
                int total = static_cast<int>(all_docs.size());
                int end = std::min(offset + limit, total);
                for (int i = offset; i < end; ++i) {
                    docs_array.push_back(DocToSpecJson(all_docs[i], ns_name));
                }

                nlohmann::json out;
                out["documents"] = std::move(docs_array);
                out["total"] = total;  // spec DocumentList.total (NOT the nested total_count)
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }
        )
    );

    // The two /documents/tasks/{task_id}* routes are registered BEFORE the generic
    // /documents/{id} routes so httplib's in-order regex dispatch never lets the
    // single-segment {id} pattern shadow a tasks path. (The patterns are already
    // disjoint by slash count, but explicit ordering keeps it obvious.)

    // GET /api/v1/documents/tasks/{task_id}/progress — task progress
    // (api/paths/documents.yaml getDocumentTaskProgress). Wraps the existing
    // DocumentTaskHandler::GetProgress (GEN-Agent §6.3 body / 404 CX_ERR_TASK_NOT_FOUND).
    server.Get(R"(/api/v1/documents/tasks/([^/]+)/progress)",
        WithAuth(auth, kPermRead,
            [&task_handler](const httplib::Request& req, httplib::Response& res,
                            const RequestContext& rctx) {
                const std::string task_id = req.matches[1];
                async::HttpResult r = task_handler.GetProgress(task_id);
                WriteJsonResponse(res, r.status, r.body, rctx.request_id);
            }
        )
    );

    // DELETE /api/v1/documents/tasks/{task_id} — task cancel
    // (api/paths/documents.yaml cancelDocumentTask). Wraps
    // DocumentTaskHandler::CancelTask (200 cancel-accepted / 404 / 423).
    server.Delete(R"(/api/v1/documents/tasks/([^/]+))",
        WithAuth(auth, kPermWrite,
            [&task_handler](const httplib::Request& req, httplib::Response& res,
                            const RequestContext& rctx) {
                const std::string task_id = req.matches[1];
                async::HttpResult r = task_handler.CancelTask(task_id);
                WriteJsonResponse(res, r.status, r.body, rctx.request_id);
            }
        )
    );

    // GET /api/v1/documents/{id}?namespace= — get one (api/paths/documents.yaml
    // getDocument). per-NS storage -> ?namespace= required.
    server.Get(R"(/api/v1/documents/([^/]+))",
        WithAuth(auth, kPermRead,
            [&pool, &handler](const httplib::Request& req, httplib::Response& res,
                              const RequestContext& rctx) {
                const std::string doc_id = req.matches[1];
                std::string ns_name;
                if (!RequireNamespaceParam(req, res, rctx.request_id, &ns_name)) return;

                resource::NamespaceFacade facade(pool, ns_name);
                Status acq = facade.Acquire();
                if (!acq.ok()) {
                    WriteJsonError(res, Status::NotFound(
                        "namespace not found: " + ns_name + ": " + acq.message()),
                        rctx.request_id);
                    return;
                }

                CortrixDoc doc;
                Status s = handler.GetDocumentStatus(facade.store(), doc_id, &doc);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }
                // a soft-deleted doc is not externally live (404 until
                // restore / hard-delete), matching the nested route.
                if (doc.status == DocStatus::kDeleted) {
                    WriteJsonError(res, Status::NotFound("document not found: " + doc_id),
                                   rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 200, DocToSpecJson(doc, ns_name), rctx.request_id);
            }
        )
    );

    // DELETE /api/v1/documents/{id}?namespace= — soft delete (api/paths/documents.yaml
    // deleteDocument; "Soft delete; recoverable via ops"). Goes through the GC
    // lifecycle doc_soft_delete (NOT a hard delete) -> 204.
    server.Delete(R"(/api/v1/documents/([^/]+))",
        WithAuth(auth, kPermWrite,
            [&pool, &handler](const httplib::Request& req, httplib::Response& res,
                              const RequestContext& rctx) {
                const std::string doc_id = req.matches[1];
                std::string ns_name;
                if (!RequireNamespaceParam(req, res, rctx.request_id, &ns_name)) return;

                resource::NamespaceFacade facade(pool, ns_name);
                Status acq = facade.Acquire();
                if (!acq.ok()) {
                    WriteJsonError(res, Status::NotFound(
                        "namespace not found: " + ns_name + ": " + acq.message()),
                        rctx.request_id);
                    return;
                }

                CortrixDoc doc;
                Status s = handler.GetDocumentStatus(facade.store(), doc_id, &doc);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }
                if (doc.status == DocStatus::kDeleted) {
                    WriteJsonError(res, Status::NotFound("document not found: " + doc_id),
                                   rctx.request_id);
                    return;
                }
                int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                int rc = facade.store().doc_soft_delete(doc_id, now_ms);
                if (rc != 0) {
                    WriteJsonError(res, Status::Internal("failed to delete document"),
                                   rctx.request_id);
                    return;
                }
                res.status = 204;  // body-less, idempotent
            }
        )
    );
}

namespace {

// Map a connector watcher status string (watching|stopped) to the spec Watcher
// `status` enum (active|paused|error). api/components/schemas.yaml#/Watcher.status.
std::string WatcherStatusToSpec(bool watching) {
    return watching ? "active" : "paused";
}

// Reshape a fan-out watcher (one directory, N subscribed namespaces) into
// the openapi Watcher object {id, path, target_namespaces[], recursive, status}.
// api/components/schemas.yaml#/Watcher. With F21 a directory fanned out to N
// namespaces is ONE Watcher carrying all N target_namespaces (the MVP emitted N
// rows under (dir, ns) keying — the spec's fan-out shape is now native).
nlohmann::json WatcherToSpecJson(const WatchInfo& w) {
    nlohmann::json j;
    j["id"] = w.id;
    j["path"] = w.directory;
    j["target_namespaces"] = w.target_namespaces;
    j["recursive"] = w.recursive;
    j["status"] = WatcherStatusToSpec(w.watching);
    return j;
}

}  // namespace

void RegisterWatchAliasRoutes(httplib::Server& server,
                              ConnectorState& state,
                              resource::INamespacePool& pool,
                              catalog::INSRouter& ins_router,
                              SPCManager& spc_mgr,
                              ApiKeyAuth& auth) {

    // The /watch aliases share the SAME DirWatcherRegistry the /connector/* routes
    // own (one registry per ConnectorState). ConnectorEnsureRegistry is idempotent,
    // so calling it here makes /watch independent of /connector route-registration
    // order: whichever registers first builds the registry, the other reuses it.
    ConnectorEnsureRegistry(state, pool, ins_router, spc_mgr);

    // POST /api/v1/watch — add/append a directory watch (api/paths/watch.yaml
    // addWatcher; body {path, target_namespaces[], recursive?}) -> 201 Watcher.
    // F21: one Subscribe() call fans `path` out to every target namespace via a
    // single OS watcher (the MVP created one watcher per (dir, ns)).
    server.Post("/api/v1/watch",
        WithAuth(auth, kPermAdmin,
            [&state](const httplib::Request& req, httplib::Response& res,
                     const RequestContext& rctx) {
                if (!state.registry) {
                    WriteJsonError(res, Status::Internal("watcher registry not initialized"),
                                   rctx.request_id);
                    return;
                }
                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch (...) {
                    WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"),
                                   rctx.request_id);
                    return;
                }
                if (!body.is_object() || !body.contains("path") || !body["path"].is_string()) {
                    WriteJsonError(res, Status::InvalidArgument("missing or invalid 'path'"),
                                   rctx.request_id);
                    return;
                }
                if (!body.contains("target_namespaces") ||
                    !body["target_namespaces"].is_array() ||
                    body["target_namespaces"].empty()) {
                    WriteJsonError(res, Status::InvalidArgument(
                        "'target_namespaces' must be a non-empty array"), rctx.request_id);
                    return;
                }
                const std::string path = body["path"].get<std::string>();

                std::vector<std::string> target_namespaces;
                for (const auto& ns : body["target_namespaces"]) {
                    if (ns.is_string() && !ns.get<std::string>().empty()) {
                        target_namespaces.push_back(ns.get<std::string>());
                    }
                }
                if (target_namespaces.empty()) {
                    WriteJsonError(res, Status::InvalidArgument(
                        "'target_namespaces' must contain non-empty strings"), rctx.request_id);
                    return;
                }
                bool recursive = true;
                if (body.contains("recursive") && body["recursive"].is_boolean()) {
                    recursive = body["recursive"].get<bool>();
                }

                Status s = state.registry->Subscribe(path, target_namespaces, recursive);
                if (!s.ok()) {  // NotFound(dir) / InvalidArgument(empty) / Internal
                    CORTRIX_LOG_ERROR("WatchAlias", "subscribe failed dir={}: {}",
                                      path, s.message());
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }
                if (!state.data_dir.empty()) state.registry->SaveState(state.data_dir);

                // 201 Watcher describing the fan-out (canonical dir + full subscriber
                // list). Resolve by the canonical id Subscribe just created.
                std::error_code cec;
                std::string canon = std::filesystem::canonical(path, cec).string();
                auto info = state.registry->GetWatch(
                    DirWatcherRegistry::MakeId(cec ? path : canon));
                nlohmann::json w = info ? WatcherToSpecJson(*info) : nlohmann::json::object();
                WriteJsonResponse(res, 201, w, rctx.request_id);
            }
        )
    );

    // GET /api/v1/watch — list watchers (api/paths/watch.yaml listWatchers) ->
    // {watchers:[Watcher]}.
    server.Get("/api/v1/watch",
        WithAuth(auth, kPermRead,
            [&state](const httplib::Request&, httplib::Response& res,
                     const RequestContext& rctx) {
                nlohmann::json watchers = nlohmann::json::array();
                if (state.registry) {
                    for (const auto& w : state.registry->ListWatches()) {
                        watchers.push_back(WatcherToSpecJson(w));
                    }
                }
                nlohmann::json out;
                out["watchers"] = std::move(watchers);
                WriteJsonResponse(res, 200, out, rctx.request_id);
            }
        )
    );

    // DELETE /api/v1/watch/{id} — remove a watch (api/paths/watch.yaml
    // deleteWatcher) -> 204. Purges the directory's indexed docs (connector
    // semantics) before stopping, matching DELETE /connector/watchers/:id.
    server.Delete(R"(/api/v1/watch/([^/]+))",
        WithAuth(auth, kPermAdmin,
            [&state](const httplib::Request& req, httplib::Response& res,
                     const RequestContext& rctx) {
                const std::string id = req.matches[1];
                if (!state.registry) {
                    WriteJsonError(res, Status::NotFound("Watcher not found: " + id),
                                   rctx.request_id);
                    return;
                }
                auto dir = state.registry->DirectoryForId(id);
                if (!dir) {
                    WriteJsonError(res, Status::NotFound("Watcher not found: " + id),
                                   rctx.request_id);
                    return;
                }
                state.registry->RemoveWatch(*dir, /*delete_docs=*/true);
                if (!state.data_dir.empty()) state.registry->SaveState(state.data_dir);
                res.status = 204;  // body-less, idempotent
            }
        )
    );

    // POST /api/v1/namespaces/{id}/reload — admin recovery for a namespace the bounded
    // startup load could not admit in time. A large NS (100MB+ store,
    // big HNSW/sparse indexes) can exceed the per-NS startup timeout and be left
    // un-admitted (its data is on disk but the pool never loaded it). This endpoint
    // calls ReloadNamespace, which loads WITHOUT the startup timeout, so the NS becomes
    // available again with no server restart and no data movement. Admin-scoped.
    server.Post(R"(/api/v1/namespaces/([^/]+)/reload)",
        WithAuth(auth, kPermAdmin,
            [&pool](const httplib::Request& req, httplib::Response& res,
                    const RequestContext& rctx) {
                const std::string ns_id = req.matches[1];
                Status s = pool.ReloadNamespace(ns_id);
                if (!s.ok()) {
                    WriteJsonError(res, s, rctx.request_id);
                    return;
                }
                nlohmann::json body;
                body["namespace_id"] = ns_id;
                body["status"] = "reloaded";
                res.status = 200;
                res.set_content(body.dump(), "application/json");
            }
        )
    );
}

}  // namespace cortrix
