#include "cortrix/server/routes/connector_routes.h"
#include "cortrix/connector/directory_importer.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"
#include "cortrix/catalog/i_ns_router.h"          // F12 INSRouter (F13 create path)
#include "cortrix/catalog/catalog_types.h"        // NSMetadata
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/resource/namespace_pool.h"      // F05 INamespacePool
#include "cortrix/spc/spc_manager.h"
#include "cortrix/logging/logging.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace cortrix {

static const char* kModule = "ConnectorRoutes";

// Generate a deterministic 8-hex-char ID from data_dir + namespace_name.
// Using a composite key allows the same directory to be watched by multiple
// namespaces independently (each gets a distinct ID).
static std::string MakeWatcherId(const std::string& data_dir, const std::string& ns_name) {
    std::string key = data_dir + ":" + ns_name;
    unsigned h = static_cast<unsigned>(std::hash<std::string>{}(key) & 0xFFFFFFFFu);
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", h);
    return std::string(buf);
}

// Serialize a WatcherEntry to JSON
static nlohmann::json WatcherToJson(const WatcherEntry& entry) {
    nlohmann::json j;
    j["id"]             = entry.id;
    j["data_dir"]       = entry.importer->GetConfig().data_dir;
    j["namespace_name"] = entry.importer->GetConfig().namespace_name;
    j["watching"]       = entry.importer->IsWatching();
    j["status"]         = entry.importer->IsWatching() ? "watching" : "stopped";
    auto stats = entry.importer->GetStats();
    j["stats"]["total_files"]       = stats.total_files;
    j["stats"]["imported"]          = stats.imported;
    j["stats"]["updated"]           = stats.updated;
    j["stats"]["skipped_unchanged"] = stats.skipped_unchanged;
    j["stats"]["skipped_filtered"]  = stats.skipped_filtered;
    j["stats"]["skipped_error"]     = stats.skipped_error;
    j["stats"]["deleted"]           = stats.deleted;
    j["stats"]["elapsed_s"]         = stats.elapsed_s;
    return j;
}

// ---------------------------------------------------------------------------
// Persistence helpers — watchers.json
// ---------------------------------------------------------------------------

// Serialize current watcher list to {state.data_dir}/watchers.json
// Caller must hold state.mu. Public (shared with the /watch aliases).
void ConnectorSaveWatchers(const ConnectorState& state) {
    if (state.data_dir.empty()) return;
    namespace fs = std::filesystem;
    fs::path path = fs::path(state.data_dir) / "watchers.json";

    nlohmann::json j;
    j["version"]  = 1;
    j["watchers"] = nlohmann::json::array();
    for (const auto& entry : state.watchers) {
        nlohmann::json w;
        w["data_dir"]       = entry.importer->GetConfig().data_dir;
        w["namespace_name"] = entry.importer->GetConfig().namespace_name;
        j["watchers"].push_back(w);
    }

    try {
        std::ofstream out(path);
        out << j.dump(2) << "\n";
        spdlog::info("[ConnectorRoutes] watchers persisted ({} entries): {}",
                     state.watchers.size(), path.string());
    } catch (const std::exception& e) {
        spdlog::warn("[ConnectorRoutes] failed to save watchers.json: {}", e.what());
    }
}

// Restore watchers from {state.data_dir}/watchers.json on startup
// Caller must hold state.mu (ConnectorAddWatcher requires it)
static void LoadAndRestoreWatchers(ConnectorState& state,
                                    catalog::INSRouter& ins_router,
                                    resource::INamespacePool& pool,
                                    SPCManager& spc_mgr) {
    if (state.data_dir.empty()) return;
    namespace fs = std::filesystem;
    fs::path path = fs::path(state.data_dir) / "watchers.json";

    std::error_code ec;
    if (!fs::exists(path, ec)) return;  // First run: no persisted watchers

    nlohmann::json j;
    try {
        std::ifstream in(path);
        in >> j;
    } catch (const std::exception& e) {
        spdlog::warn("[ConnectorRoutes] failed to load watchers.json: {}", e.what());
        return;
    }

    int restored = 0;
    for (const auto& w : j.value("watchers", nlohmann::json::array())) {
        std::string data_dir = w.value("data_dir", "");
        std::string ns_name  = w.value("namespace_name", "default");
        if (data_dir.empty()) continue;

        if (!fs::exists(data_dir, ec) || !fs::is_directory(data_dir, ec)) {
            spdlog::warn("[ConnectorRoutes] persisted watcher dir gone, skipping: {}", data_dir);
            continue;
        }

        // autostart=false: let the main.cpp startup loop call Start() to avoid double-scan
        Status s = ConnectorAddWatcher(state, ins_router, pool, spc_mgr, data_dir, ns_name,
                                nullptr, /*autostart=*/false);
        if (s.ok()) {
            restored++;
            spdlog::info("[ConnectorRoutes] watcher registered (pending start): dir={}, ns={}", data_dir, ns_name);
        } else {
            spdlog::warn("[ConnectorRoutes] restore failed dir={}: {}", data_dir, s.message());
        }
    }
    if (restored > 0)
        spdlog::info("[ConnectorRoutes] {} watcher(s) restored from watchers.json", restored);
}

// ---------------------------------------------------------------------------
// Shared helper: create + start a watcher (replaces same (dir, ns) if present).
// Caller must hold state.mu. Public (shared with the /watch aliases).
Status ConnectorAddWatcher(ConnectorState& state,
                            catalog::INSRouter& ins_router,
                            resource::INamespacePool& pool,
                            SPCManager& spc_mgr,
                            const std::string& data_dir,
                            const std::string& ns_name,
                            WatcherEntry** out_entry,
                            bool autostart) {
    // Auto-create namespace (idempotent), routed through the F12 catalog (F13):
    // the catalog INSERT admits the NS into the F05 pool, so the per-op façade the
    // importer acquires below resolves. An existing NS is not an error here.
    {
        catalog::NSMetadata meta;
        meta.namespace_id = ns_name;
        meta.name         = ns_name;
        Status ns_s = ins_router.CreateNamespace(meta);
        if (!ns_s.ok() && ns_s.code() != StatusCode::kAlreadyExists) {
            return ns_s;
        }
    }

    std::string id = MakeWatcherId(data_dir, ns_name);

    // Stop & remove existing watcher for same data_dir (update in-place)
    for (auto it = state.watchers.begin(); it != state.watchers.end(); ++it) {
        if (it->id == id) {
            it->importer->Stop();
            state.watchers.erase(it);
            break;
        }
    }

    WatchDirConfig config;
    config.data_dir       = data_dir;
    config.namespace_name = ns_name;
    config.watch_enabled  = true;

    auto importer = std::make_unique<DirectoryImporter>(config, pool, spc_mgr);
    if (autostart) {
        Status s = importer->Start();
        if (!s.ok()) return s;
    }

    state.watchers.push_back({id, std::move(importer)});
    if (out_entry) *out_entry = &state.watchers.back();
    return Status::Ok();
}

void RegisterConnectorRoutes(httplib::Server& server,
                              ConnectorState& state,
                              cortrix::resource::INamespacePool& pool,
                              cortrix::catalog::INSRouter& ins_router,
                              NamespaceManager& /*meta_mgr*/,
                              SPCManager& spc_mgr,
                              ApiKeyAuth& /*auth*/) {

    // Restore persisted watchers from watchers.json (no-op on first run)
    {
        std::lock_guard<std::mutex> lock(state.mu);
        LoadAndRestoreWatchers(state, ins_router, pool, spc_mgr);
    }

    // ----------------------------------------------------------------
    // Multi-watcher endpoints
    // ----------------------------------------------------------------

    // GET /api/v1/connector/watchers — list all configured watchers
    server.Get("/api/v1/connector/watchers", NoAuth(
        [&state](const httplib::Request&, httplib::Response& res,
                 const RequestContext& rctx) {
            std::lock_guard<std::mutex> lock(state.mu);
            nlohmann::json body;
            body["watchers"] = nlohmann::json::array();
            for (const auto& entry : state.watchers) {
                body["watchers"].push_back(WatcherToJson(entry));
            }
            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));

    // POST /api/v1/connector/watchers — add a new watcher (keeps existing ones)
    server.Post("/api/v1/connector/watchers", NoAuth(
        [&state, &pool, &ins_router, &spc_mgr](const httplib::Request& req,
                                                 httplib::Response& res,
                                                 const RequestContext& rctx) {
            nlohmann::json body;
            try {
                body = nlohmann::json::parse(req.body);
            } catch (...) {
                WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"),
                               rctx.request_id);
                return;
            }

            std::string data_dir = body.value("data_dir", "");
            std::string ns_name  = body.value("namespace_name", "default");

            if (data_dir.empty()) {
                WriteJsonError(res, Status::InvalidArgument("data_dir is required"),
                               rctx.request_id);
                return;
            }

            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(data_dir, ec) || !fs::is_directory(data_dir, ec)) {
                WriteJsonError(res,
                    Status::NotFound("Directory not found: " + data_dir),
                    rctx.request_id);
                return;
            }

            std::lock_guard<std::mutex> lock(state.mu);
            WatcherEntry* entry = nullptr;
            Status s = ConnectorAddWatcher(state, ins_router, pool, spc_mgr,
                                    data_dir, ns_name, &entry);
            if (!s.ok()) {
                CORTRIX_LOG_ERROR(kModule, "failed to add watcher: {}", s.message());
                WriteJsonError(res, s, rctx.request_id);
                return;
            }

            CORTRIX_LOG_INFO(kModule, "watcher added: dir={}, ns={}, id={}",
                            data_dir, ns_name, entry->id);
            ConnectorSaveWatchers(state);

            nlohmann::json resp = WatcherToJson(*entry);
            resp["message"] = "Watcher added and monitoring started";
            WriteJsonResponse(res, 200, resp, rctx.request_id);
        }
    ));

    // DELETE /api/v1/connector/watchers/:id — remove a specific watcher
    server.Delete("/api/v1/connector/watchers/:id", NoAuth(
        [&state](const httplib::Request& req, httplib::Response& res,
                 const RequestContext& rctx) {
            std::string id = req.path_params.count("id")
                             ? req.path_params.at("id") : "";
            if (id.empty()) {
                WriteJsonError(res, Status::InvalidArgument("id is required"),
                               rctx.request_id);
                return;
            }

            std::lock_guard<std::mutex> lock(state.mu);
            for (auto it = state.watchers.begin(); it != state.watchers.end(); ++it) {
                if (it->id == id) {
                    // Delete all indexed documents from this directory before stopping
                    int deleted_docs = it->importer->DeleteAllDocs();
                    it->importer->Stop();
                    std::string removed_dir = it->importer->GetConfig().data_dir;
                    state.watchers.erase(it);
                    CORTRIX_LOG_INFO(kModule, "watcher removed: id={}, deleted_docs={}", id, deleted_docs);
                    ConnectorSaveWatchers(state);
                    nlohmann::json resp;
                    resp["message"] = "Watcher removed";
                    resp["deleted_docs"] = deleted_docs;
                    resp["data_dir"] = removed_dir;
                    WriteJsonResponse(res, 200, resp, rctx.request_id);
                    return;
                }
            }
            WriteJsonError(res, Status::NotFound("Watcher not found: " + id),
                           rctx.request_id);
        }
    ));

    // POST /api/v1/connector/watchers/:id/scan — scan a specific watcher
    server.Post("/api/v1/connector/watchers/:id/scan", NoAuth(
        [&state](const httplib::Request& req, httplib::Response& res,
                 const RequestContext& rctx) {
            std::string id = req.path_params.count("id")
                             ? req.path_params.at("id") : "";

            std::lock_guard<std::mutex> lock(state.mu);
            for (auto& entry : state.watchers) {
                if (entry.id == id) {
                    entry.importer->Stop();
                    Status s = entry.importer->Start();
                    if (!s.ok()) {
                        WriteJsonError(res, s, rctx.request_id);
                        return;
                    }
                    nlohmann::json resp = WatcherToJson(entry);
                    resp["message"] = "Scan completed";
                    WriteJsonResponse(res, 200, resp, rctx.request_id);
                    return;
                }
            }
            WriteJsonError(res, Status::NotFound("Watcher not found: " + id),
                           rctx.request_id);
        }
    ));

    // ----------------------------------------------------------------
    // Backward-compat single-watcher endpoints
    // ----------------------------------------------------------------

    // GET /api/v1/connector/status — first watcher status (backward compat)
    server.Get("/api/v1/connector/status", NoAuth(
        [&state](const httplib::Request&, httplib::Response& res,
                 const RequestContext& rctx) {
            std::lock_guard<std::mutex> lock(state.mu);
            nlohmann::json body;

            if (state.watchers.empty()) {
                body["enabled"]       = false;
                body["watching"]      = false;
                body["status"]        = "not_configured";
                body["message"]       = "No watchers configured";
                body["watcher_count"] = 0;
            } else {
                const auto& entry = state.watchers.front();
                bool watching = entry.importer->IsWatching();
                body["enabled"]         = true;
                body["watching"]        = watching;
                body["status"]          = watching ? "watching" : "stopped";
                body["watch_dir"]       = entry.importer->GetConfig().data_dir;
                body["namespace_name"]  = entry.importer->GetConfig().namespace_name;
                body["watcher_count"]   = static_cast<int>(state.watchers.size());
            }

            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));

    // POST /api/v1/connector/watch — backward compat: replace ALL watchers with one
    server.Post("/api/v1/connector/watch", NoAuth(
        [&state, &pool, &ins_router, &spc_mgr](const httplib::Request& req,
                                                  httplib::Response& res,
                                                  const RequestContext& rctx) {
            nlohmann::json body;
            try {
                body = nlohmann::json::parse(req.body);
            } catch (...) {
                WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"),
                               rctx.request_id);
                return;
            }

            std::string data_dir = body.value("data_dir", "");
            std::string ns_name  = body.value("namespace_name", "default");

            if (data_dir.empty()) {
                WriteJsonError(res, Status::InvalidArgument("data_dir is required"),
                               rctx.request_id);
                return;
            }

            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(data_dir, ec) || !fs::is_directory(data_dir, ec)) {
                WriteJsonError(res, Status::NotFound("Directory not found: " + data_dir),
                               rctx.request_id);
                return;
            }

            std::lock_guard<std::mutex> lock(state.mu);

            // Stop + clear ALL existing watchers, then add the new one
            for (auto& entry : state.watchers) {
                entry.importer->Stop();
            }
            state.watchers.clear();

            WatcherEntry* entry = nullptr;
            Status s = ConnectorAddWatcher(state, ins_router, pool, spc_mgr,
                                    data_dir, ns_name, &entry);
            if (!s.ok()) {
                CORTRIX_LOG_ERROR(kModule, "failed to start directory importer: {}", s.message());
                WriteJsonError(res, s, rctx.request_id);
                return;
            }

            CORTRIX_LOG_INFO(kModule, "directory importer started (compat): dir={}, ns={}",
                            data_dir, ns_name);
            ConnectorSaveWatchers(state);

            nlohmann::json resp;
            resp["message"]        = "Watch directory configured and monitoring started";
            resp["watch_dir"]      = data_dir;
            resp["namespace_name"] = ns_name;
            resp["watching"]       = true;
            WriteJsonResponse(res, 200, resp, rctx.request_id);
        }
    ));

    // GET /api/v1/connector/stats — aggregate stats from all watchers
    server.Get("/api/v1/connector/stats", NoAuth(
        [&state](const httplib::Request&, httplib::Response& res,
                 const RequestContext& rctx) {
            std::lock_guard<std::mutex> lock(state.mu);
            nlohmann::json body;

            if (state.watchers.empty()) {
                body["enabled"] = false;
                body["message"] = "No watchers configured";
                WriteJsonResponse(res, 200, body, rctx.request_id);
                return;
            }

            int total_files = 0, imported = 0, updated = 0;
            int skipped_unchanged = 0, skipped_filtered = 0, skipped_error = 0, deleted = 0;
            double elapsed_s = 0.0;
            bool any_watching = false;

            for (const auto& entry : state.watchers) {
                auto stats = entry.importer->GetStats();
                total_files       += stats.total_files;
                imported          += stats.imported;
                updated           += stats.updated;
                skipped_unchanged += stats.skipped_unchanged;
                skipped_filtered  += stats.skipped_filtered;
                skipped_error     += stats.skipped_error;
                deleted           += stats.deleted;
                elapsed_s         += stats.elapsed_s;
                if (entry.importer->IsWatching()) any_watching = true;
            }

            body["enabled"]           = true;
            body["watching"]          = any_watching;
            body["watcher_count"]     = static_cast<int>(state.watchers.size());
            body["total_files"]       = total_files;
            body["imported"]          = imported;
            body["updated"]           = updated;
            body["skipped_unchanged"] = skipped_unchanged;
            body["skipped_filtered"]  = skipped_filtered;
            body["skipped_error"]     = skipped_error;
            body["deleted"]           = deleted;
            body["elapsed_s"]         = elapsed_s;

            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));

    // POST /api/v1/connector/scan — scan all watchers
    server.Post("/api/v1/connector/scan", NoAuth(
        [&state](const httplib::Request&, httplib::Response& res,
                 const RequestContext& rctx) {
            std::lock_guard<std::mutex> lock(state.mu);

            if (state.watchers.empty()) {
                WriteJsonError(res,
                    Status::InvalidArgument("No watchers configured. Use POST /api/v1/connector/watchers first."),
                    rctx.request_id);
                return;
            }

            for (auto& entry : state.watchers) {
                entry.importer->Stop();
                entry.importer->Start();
            }

            int total_files = 0, imported = 0, updated = 0;
            int skipped_unchanged = 0, skipped_filtered = 0, skipped_error = 0, deleted = 0;
            double elapsed_s = 0.0;

            for (const auto& entry : state.watchers) {
                auto stats = entry.importer->GetStats();
                total_files       += stats.total_files;
                imported          += stats.imported;
                updated           += stats.updated;
                skipped_unchanged += stats.skipped_unchanged;
                skipped_filtered  += stats.skipped_filtered;
                skipped_error     += stats.skipped_error;
                deleted           += stats.deleted;
                elapsed_s         += stats.elapsed_s;
            }

            nlohmann::json body;
            body["message"]           = "Scan completed";
            body["imported"]          = imported;
            body["updated"]           = updated;
            body["skipped_unchanged"] = skipped_unchanged;
            body["skipped_filtered"]  = skipped_filtered;
            body["skipped_error"]     = skipped_error;
            body["deleted"]           = deleted;
            body["total_files"]       = total_files;
            body["elapsed_s"]         = elapsed_s;
            WriteJsonResponse(res, 200, body, rctx.request_id);
        }
    ));

    // ----------------------------------------------------------------
    // Directory picker (unchanged)
    // ----------------------------------------------------------------

    // GET /api/v1/browse?path=<dir>
    server.Get("/api/v1/browse", NoAuth(
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
