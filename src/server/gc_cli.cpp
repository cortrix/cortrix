#include "cortrix/server/gc_cli.h"

#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/config/config.h"

namespace cortrix::server {

namespace {

// Parse `--config <path>` out of argv (same flag the server honours) so the CLI
// resolves the data dir + gc windows from the same config the server would use.
std::string ParseConfigPath(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) return argv[i + 1];
    }
    return "";
}

void PrintJson(const nlohmann::json& j) { std::printf("%s\n", j.dump(2).c_str()); }

nlohmann::json StatusJson(const catalog::gc::GcStatusSnapshot& s) {
    nlohmann::json j;
    j["status"] = s.running ? "running" : "idle";
    j["soft_deleted_count"] = s.soft_deleted_count;
    j["reclaimable_bytes"] = s.reclaimable_bytes;
    j["last_gc_at"] = s.last_gc_at_ms.has_value() ? nlohmann::json(*s.last_gc_at_ms)
                                                  : nlohmann::json(nullptr);
    return j;
}

nlohmann::json ReportJson(const catalog::gc::GcRunReport& r) {
    return {{"hard_deleted", r.hard_deleted},
            {"blobs_unlinked", r.blobs_unlinked},
            {"hit_run_cap", r.hit_run_cap}};
}

}  // namespace

std::optional<int> MaybeRunGcCli(int argc, char* argv[]) {
    if (argc < 2) return std::nullopt;
    const std::string cmd = argv[1];
    const bool is_gc =
        (cmd == "gc-status" || cmd == "gc-run" || cmd == "purge" || cmd == "restore");
    if (!is_gc) return std::nullopt;

    const std::string config_path = ParseConfigPath(argc, argv);
    auto config = cortrix::LoadConfig(config_path);

    catalog::CatalogDb catalog;
    const std::string catalog_path = config.ns.data_dir + "/catalog.db";
    auto cs = catalog.Open(catalog_path);
    if (!cs.ok()) {
        std::fprintf(stderr, "gc-cli: failed to open %s: %s\n", catalog_path.c_str(),
                     cs.message().c_str());
        return 1;
    }
    catalog::gc::LocalBlobGcSink sink(config.ns.data_dir + "/blobs");
    catalog::gc::GcManager gc(catalog.db(), &sink, config.gc);

    if (cmd == "gc-status") {
        auto s = gc.GetStatus();
        if (!s.ok()) {
            std::fprintf(stderr, "gc-cli: status failed: %s\n", s.status().message().c_str());
            return 1;
        }
        PrintJson(StatusJson(s.value()));
        return 0;
    }
    if (cmd == "gc-run" || cmd == "purge") {
        auto r = (cmd == "purge") ? gc.Purge() : gc.RunOnce();
        if (!r.ok()) {
            std::fprintf(stderr, "gc-cli: %s failed: %s\n", cmd.c_str(),
                         r.status().message().c_str());
            return 1;
        }
        PrintJson(ReportJson(r.value()));
        return 0;
    }
    // restore <doc_id> [doc_id ...]
    std::vector<std::string> doc_ids;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config") { ++i; continue; }  // skip the flag + its value
        doc_ids.push_back(a);
    }
    if (doc_ids.empty()) {
        std::fprintf(stderr, "gc-cli: restore requires at least one doc_id\n");
        return 2;
    }
    auto r = gc.Restore(doc_ids);
    if (!r.ok()) {
        std::fprintf(stderr, "gc-cli: restore failed: %s\n", r.status().message().c_str());
        return 1;
    }
    nlohmann::json succeeded = nlohmann::json::array();
    for (const auto& id : r.value().succeeded) succeeded.push_back(id);
    nlohmann::json failed = nlohmann::json::array();
    for (const auto& [id, code] : r.value().failed) {
        failed.push_back({{"doc_id", id}, {"error_code", code}});
    }
    PrintJson({{"succeeded", succeeded}, {"failed", failed}});
    return 0;
}

}  // namespace cortrix::server
