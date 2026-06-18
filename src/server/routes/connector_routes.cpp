#include "cortrix/server/routes/connector_routes.h"
#include "cortrix/connector/dir_watcher_registry.h"
#include "cortrix/connector/directory_importer.h"  // ImportStats
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"
#include "cortrix/catalog/i_ns_router.h"          // F12 INSRouter (F13 create path)
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/resource/namespace_pool.h"      // F05 INamespacePool
#include "cortrix/spc/spc_manager.h"
#include "cortrix/logging/logging.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <vector>

namespace cortrix {

static const char* kModule = "ConnectorRoutes";

namespace {

// Serialize one ImportStats block into a JSON object (shared by the per-NS and
// the backward-compat aggregate responses).
nlohmann::json StatsToJson(const ImportStats& s) {
    nlohmann::json j;
    j["total_files"]       = s.total_files;
    j["imported"]          = s.imported;
    j["updated"]           = s.updated;
    j["skipped_unchanged"] = s.skipped_unchanged;
    j["skipped_filtered"]  = s.skipped_filtered;
    j["skipped_error"]     = s.skipped_error;
    j["deleted"]           = s.deleted;
    j["elapsed_s"]         = s.elapsed_s;
    return j;
}

// Serialize one watcher (a directory + its fan-out subscribers) to the F21 GET
// shape: id / directory / target_namespaces[] / subscriber_count / watching /
// recursive + nested per-NS stats.
nlohmann::json WatchToJson(const DirWatcherRegistry& reg, const WatchInfo& w) {
    nlohmann::json j;
    j["id"]                = w.id;
    j["directory"]         = w.directory;
    j["target_namespaces"] = w.target_namespaces;
    j["subscriber_count"]  = w.subscriber_count;
    j["watching"]          = w.watching;
    j["status"]            = w.watching ? "watching" : "stopped";
    j["recursive"]         = w.recursive;
    nlohmann::json stats = nlohmann::json::object();
    for (const auto& [ns, st] : reg.GetStats(w.directory)) {
        stats[ns] = StatsToJson(st);
    }
    j["stats"] = std::move(stats);
    return j;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registry lifecycle
// ---------------------------------------------------------------------------

void ConnectorEnsureRegistry(ConnectorState& state,
                             resource::INamespacePool& pool,
                             catalog::INSRouter& ins_router,
                             SPCManager& spc_mgr) {
    if (state.registry) return;  // idempotent: shared by /connector + /watch
    state.registry =
        std::make_unique<DirWatcherRegistry>(pool, ins_router, spc_mgr);
    // Restore persisted watchers (v1 layouts auto-migrate to v2). autostart is
    // deferred: bootstrap calls StartAll() once the server is ready so OS
    // watching + the initial scan do not run during route registration.
    if (!state.data_dir.empty()) {
        Status s = state.registry->LoadState(state.data_dir);
        if (!s.ok()) {
            CORTRIX_LOG_WARN(kModule, "watchers.json restore failed: {}",
                             s.message());
        }
    }
}

void RegisterConnectorRoutes(httplib::Server& server,
                              ConnectorState& state,
                              cortrix::resource::INamespacePool& pool,
                              cortrix::catalog::INSRouter& ins_router,
                              SPCManager& spc_mgr,
                              ApiKeyAuth& auth) {

    ConnectorEnsureRegistry(state, pool, ins_router, spc_mgr);
    DirWatcherRegistry& reg = *state.registry;

    // ----------------------------------------------------------------
    // Multi-watcher endpoints (F21 fan-out)
    // ----------------------------------------------------------------

    // GET /api/v1/connector/watchers — list all watchers with per-NS stats.
    server.Get("/api/v1/connector/watchers", WithAuth(auth, kPermAdmin,
        [&reg](const httplib::Request&, httplib::Response& res,
               const RequestContext& rctx) {
            nlohmann::json body;
            body["watchers"] = nlohmann::json::array();
            for (const auto& w : reg.ListWatches()) {
                body["watchers"].push_back(WatchToJson(reg, w));
            }
            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));

    // POST /api/v1/connector/watchers — subscribe namespace(s) to a directory.
    // F21: accepts `target_namespaces: [...]` (fan-out); the MVP single
    // `namespace_name` is still honored (mapped to a one-element array).
    server.Post("/api/v1/connector/watchers", WithAuth(auth, kPermAdmin,
        [&reg, &state](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
            nlohmann::json body;
            try {
                body = nlohmann::json::parse(req.body);
            } catch (...) {
                WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"),
                               rctx.request_id);
                return;
            }

            // `path` is the F21 field; `data_dir` is the MVP alias.
            std::string data_dir = body.value("path", body.value("data_dir", std::string{}));
            if (data_dir.empty()) {
                WriteJsonError(res, Status::InvalidArgument("path (or data_dir) is required"),
                               rctx.request_id);
                return;
            }

            // target_namespaces[] (F21) OR namespace_name (MVP, default "default").
            std::vector<std::string> target_namespaces;
            if (body.contains("target_namespaces") &&
                body["target_namespaces"].is_array()) {
                for (const auto& ns : body["target_namespaces"]) {
                    if (ns.is_string() && !ns.get<std::string>().empty()) {
                        target_namespaces.push_back(ns.get<std::string>());
                    }
                }
                if (target_namespaces.empty()) {
                    WriteJsonError(res, Status::InvalidArgument(
                        "target_namespaces must contain non-empty strings"),
                        rctx.request_id);
                    return;
                }
            } else {
                target_namespaces.push_back(body.value("namespace_name", "default"));
            }

            bool recursive = body.value("recursive", true);

            Status s = reg.Subscribe(data_dir, target_namespaces, recursive);
            if (!s.ok()) {
                CORTRIX_LOG_ERROR(kModule, "failed to subscribe dir={}: {}",
                                  data_dir, s.message());
                WriteJsonError(res, s, rctx.request_id);
                return;
            }
            if (!state.data_dir.empty()) reg.SaveState(state.data_dir);

            // Return the watcher's F21 view (canonical dir + full subscriber list).
            // Subscribe succeeded so the dir resolves; canonicalize defensively.
            std::error_code cec;
            std::string canon = std::filesystem::canonical(data_dir, cec).string();
            std::string id = DirWatcherRegistry::MakeId(cec ? data_dir : canon);
            auto info = reg.GetWatch(id);
            nlohmann::json resp = info ? WatchToJson(reg, *info) : nlohmann::json::object();
            resp["message"] = "Watcher subscribed and monitoring started";
            CORTRIX_LOG_INFO(kModule, "watcher subscribed: dir={}, subscribers={}",
                             data_dir, target_namespaces.size());
            WriteJsonResponse(res, 200, resp, rctx.request_id);
        }
    ));

    // DELETE /api/v1/connector/watchers/:id — remove a watcher (all NS) + purge docs.
    server.Delete("/api/v1/connector/watchers/:id", WithAuth(auth, kPermAdmin,
        [&reg, &state](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
            std::string id = req.path_params.count("id")
                             ? req.path_params.at("id") : "";
            if (id.empty()) {
                WriteJsonError(res, Status::InvalidArgument("id is required"),
                               rctx.request_id);
                return;
            }

            auto dir = reg.DirectoryForId(id);
            if (!dir) {
                WriteJsonError(res, Status::NotFound("Watcher not found: " + id),
                               rctx.request_id);
                return;
            }
            int deleted_docs = 0;
            Status s = reg.RemoveWatch(*dir, /*delete_docs=*/true, &deleted_docs);
            if (!s.ok()) {
                WriteJsonError(res, s, rctx.request_id);
                return;
            }
            if (!state.data_dir.empty()) reg.SaveState(state.data_dir);
            CORTRIX_LOG_INFO(kModule, "watcher removed: id={}, deleted_docs={}",
                             id, deleted_docs);
            nlohmann::json resp;
            resp["message"]      = "Watcher removed";
            resp["deleted_docs"] = deleted_docs;
            resp["data_dir"]     = *dir;
            WriteJsonResponse(res, 200, resp, rctx.request_id);
        }
    ));

    // DELETE /api/v1/connector/watchers/:id/namespaces/:ns — F21: unsubscribe a
    // single namespace. The last unsubscribe destroys the watcher (no doc purge
    // on a per-NS unsubscribe — that is the whole-watcher DELETE's job).
    server.Delete("/api/v1/connector/watchers/:id/namespaces/:ns", WithAuth(auth, kPermAdmin,
        [&reg, &state](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
            std::string id = req.path_params.count("id")
                             ? req.path_params.at("id") : "";
            std::string ns = req.path_params.count("ns")
                             ? req.path_params.at("ns") : "";
            if (id.empty() || ns.empty()) {
                WriteJsonError(res, Status::InvalidArgument("id and ns are required"),
                               rctx.request_id);
                return;
            }

            auto dir = reg.DirectoryForId(id);
            if (!dir) {
                WriteJsonError(res, Status::NotFound("Watcher not found: " + id),
                               rctx.request_id);
                return;
            }
            Status s = reg.Unsubscribe(*dir, ns);
            if (!s.ok()) {
                WriteJsonError(res, s, rctx.request_id);  // NotFound if NS not subscribed
                return;
            }
            if (!state.data_dir.empty()) reg.SaveState(state.data_dir);

            // After unsubscribe the watcher is gone iff GetWatch(id) is now empty.
            auto info = reg.GetWatch(id);
            bool removed = !info.has_value();
            int remaining = info ? info->subscriber_count : 0;
            CORTRIX_LOG_INFO(kModule,
                "namespace unsubscribed: id={}, ns={}, remaining={}, watcher_removed={}",
                id, ns, remaining, removed);
            nlohmann::json resp;
            resp["status"]               = "ok";
            resp["remaining_subscribers"] = remaining;
            resp["watcher_removed"]      = removed;
            WriteJsonResponse(res, 200, resp, rctx.request_id);
        }
    ));

    // POST /api/v1/connector/watchers/:id/scan — trigger a fan-out rescan.
    server.Post("/api/v1/connector/watchers/:id/scan", WithAuth(auth, kPermAdmin,
        [&reg](const httplib::Request& req, httplib::Response& res,
               const RequestContext& rctx) {
            std::string id = req.path_params.count("id")
                             ? req.path_params.at("id") : "";
            auto dir = reg.DirectoryForId(id);
            if (!dir) {
                WriteJsonError(res, Status::NotFound("Watcher not found: " + id),
                               rctx.request_id);
                return;
            }
            Status s = reg.TriggerScan(*dir);
            if (!s.ok()) {
                WriteJsonError(res, s, rctx.request_id);
                return;
            }
            auto info = reg.GetWatch(id);
            nlohmann::json resp = info ? WatchToJson(reg, *info) : nlohmann::json::object();
            resp["message"] = "Scan completed";
            WriteJsonResponse(res, 200, resp, rctx.request_id);
        }
    ));

    // ----------------------------------------------------------------
    // Backward-compat single-watcher endpoints
    // ----------------------------------------------------------------

    // GET /api/v1/connector/status — first watcher status (backward compat).
    server.Get("/api/v1/connector/status", WithAuth(auth, kPermAdmin,
        [&reg](const httplib::Request&, httplib::Response& res,
               const RequestContext& rctx) {
            nlohmann::json body;
            auto watches = reg.ListWatches();
            if (watches.empty()) {
                body["enabled"]       = false;
                body["watching"]      = false;
                body["status"]        = "not_configured";
                body["message"]       = "No watchers configured";
                body["watcher_count"] = 0;
            } else {
                const auto& w = watches.front();
                body["enabled"]        = true;
                body["watching"]       = w.watching;
                body["status"]         = w.watching ? "watching" : "stopped";
                body["watch_dir"]      = w.directory;
                // Compat: surface the first subscriber as namespace_name.
                body["namespace_name"] = w.target_namespaces.empty()
                                         ? "" : w.target_namespaces.front();
                body["watcher_count"]  = static_cast<int>(watches.size());
            }
            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));

    // POST /api/v1/connector/watch — backward compat: replace ALL watchers with one.
    server.Post("/api/v1/connector/watch", WithAuth(auth, kPermAdmin,
        [&reg, &state](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
            nlohmann::json body;
            try {
                body = nlohmann::json::parse(req.body);
            } catch (...) {
                WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"),
                               rctx.request_id);
                return;
            }

            std::string data_dir = body.value("data_dir", body.value("path", std::string{}));
            std::string ns_name  = body.value("namespace_name", "default");
            if (data_dir.empty()) {
                WriteJsonError(res, Status::InvalidArgument("data_dir is required"),
                               rctx.request_id);
                return;
            }

            // Replace ALL watchers with the single requested one (compat
            // semantics). RemoveWatch each existing directory first.
            for (const auto& w : reg.ListWatches()) {
                reg.RemoveWatch(w.directory);
            }
            Status s = reg.Subscribe(data_dir, {ns_name}, /*recursive=*/true);
            if (!s.ok()) {
                CORTRIX_LOG_ERROR(kModule, "failed to start watch (compat): {}", s.message());
                WriteJsonError(res, s, rctx.request_id);
                return;
            }
            if (!state.data_dir.empty()) reg.SaveState(state.data_dir);
            CORTRIX_LOG_INFO(kModule, "watch configured (compat): dir={}, ns={}",
                             data_dir, ns_name);

            nlohmann::json resp;
            resp["message"]        = "Watch directory configured and monitoring started";
            resp["watch_dir"]      = data_dir;
            resp["namespace_name"] = ns_name;
            resp["watching"]       = true;
            WriteJsonResponse(res, 200, resp, rctx.request_id);
        }
    ));

    // GET /api/v1/connector/stats — aggregate stats across all watchers.
    server.Get("/api/v1/connector/stats", WithAuth(auth, kPermAdmin,
        [&reg](const httplib::Request&, httplib::Response& res,
               const RequestContext& rctx) {
            auto watches = reg.ListWatches();
            nlohmann::json body;
            if (watches.empty()) {
                body["enabled"] = false;
                body["message"] = "No watchers configured";
                WriteJsonResponse(res, 200, body, rctx.request_id);
                return;
            }
            bool any_watching = false;
            for (const auto& w : watches) any_watching |= w.watching;

            ImportStats agg = reg.AggregateStats();
            body              = StatsToJson(agg);  // total_files / imported / ...
            body["enabled"]       = true;
            body["watching"]      = any_watching;
            body["watcher_count"] = static_cast<int>(watches.size());
            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));

    // POST /api/v1/connector/scan — rescan all watchers.
    server.Post("/api/v1/connector/scan", WithAuth(auth, kPermAdmin,
        [&reg](const httplib::Request&, httplib::Response& res,
               const RequestContext& rctx) {
            auto watches = reg.ListWatches();
            if (watches.empty()) {
                WriteJsonError(res,
                    Status::InvalidArgument("No watchers configured. Use POST /api/v1/connector/watchers first."),
                    rctx.request_id);
                return;
            }
            for (const auto& w : watches) {
                reg.TriggerScan(w.directory);
            }
            ImportStats agg = reg.AggregateStats();
            nlohmann::json body = StatsToJson(agg);
            body["message"] = "Scan completed";
            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));

    // ----------------------------------------------------------------
    // Directory picker (unchanged)
    // ----------------------------------------------------------------

    // GET /api/v1/browse?path=<dir>
    // ADMIN-gated: this lists arbitrary server filesystem directories (the watch_dir
    // picker), so an unauthenticated caller could enumerate the host fs (info leak).
    // Listing the server fs is an admin-level operation; in the no-auth CE deployment
    // mode WithAuth grants admin, so the directory-picker UI flow is unaffected.
    server.Get("/api/v1/browse", WithAuth(auth, kPermAdmin,
        [](const httplib::Request& req, httplib::Response& res,
           const RequestContext& rctx) {
            namespace fs = std::filesystem;

            std::string requested = req.get_param_value("path");
            if (requested.empty()) {
                const char* home = std::getenv("HOME");
                requested = home ? home : "/";
            }

            std::error_code ec;
            fs::path dir = fs::weakly_canonical(requested, ec);
            if (ec || !fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
                WriteJsonError(res,
                    Status::NotFound("Directory not found: " + requested),
                    rctx.request_id);
                return;
            }

            nlohmann::json body;
            body["path"]    = dir.string();
            body["parent"]  = (dir.has_parent_path() && dir.parent_path() != dir)
                                ? dir.parent_path().string() : "";
            body["entries"] = nlohmann::json::array();

            std::error_code iter_ec;
            fs::directory_iterator it(dir, iter_ec);
            if (!iter_ec) {
                for (const auto& entry : it) {
                    std::error_code entry_ec;
                    if (!entry.is_directory(entry_ec) || entry_ec) continue;
                    std::string name = entry.path().filename().string();
                    if (name.empty() || name[0] == '.') continue;
                    nlohmann::json e;
                    e["name"] = name;
                    e["path"] = entry.path().string();
                    body["entries"].push_back(e);
                }
            }

            auto& entries = body["entries"];
            std::sort(entries.begin(), entries.end(),
                [](const nlohmann::json& a, const nlohmann::json& b) {
                    return a["name"].get<std::string>() < b["name"].get<std::string>();
                });

            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));
}

}  // namespace cortrix
