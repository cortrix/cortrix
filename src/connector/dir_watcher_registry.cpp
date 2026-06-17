#include "cortrix/connector/dir_watcher_registry.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>

#include <nlohmann/json.hpp>

#include "cortrix/catalog/catalog_types.h"   // NSMetadata
#include "cortrix/catalog/i_ns_router.h"      // F13 INSRouter (create + F05 admit)
#include "cortrix/connector/file_filter.h"
#include "cortrix/logging/logging.h"

namespace cortrix {

static const char* kModule = "DirWatcherRegistry";

// Fixed ignore patterns for Phase 1 (TD-WATCHER-OPTIONS makes these per-NS in
// Phase 2). Mirrors WatchDirConfig's defaults — the single source for fan-out
// filtering and for each importer's own filter.
static const std::vector<std::string> kIgnorePatterns = {
    ".*", "*~", "*.tmp", "*.swp", "#*#"};

namespace {

/// Canonicalize a directory path. Returns empty string if it does not exist or
/// is not a directory.
std::string CanonicalDir(const std::string& directory) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(directory, ec) || !fs::is_directory(directory, ec)) {
        return {};
    }
    auto canonical = fs::canonical(directory, ec);
    if (ec) return {};
    return canonical.string();
}

/// Build a WatchDirConfig for one (directory, namespace) importer. The registry
/// owns OS watching, so watch_enabled is always false here.
WatchDirConfig MakeImporterConfig(const std::string& canonical_dir,
                                  const std::string& ns) {
    WatchDirConfig cfg;
    cfg.data_dir = canonical_dir;
    cfg.namespace_name = ns;
    cfg.watch_enabled = false;  // F21: OS watcher owned by the registry
    cfg.ignore_patterns = kIgnorePatterns;
    return cfg;
}

}  // namespace

// --- ctor / dtor ---

DirWatcherRegistry::DirWatcherRegistry(cortrix::resource::INamespacePool& pool,
                                       cortrix::catalog::INSRouter& ins_router,
                                       SPCManager& spc_mgr)
    : pool_(pool), ins_router_(ins_router), spc_mgr_(spc_mgr) {}

DirWatcherRegistry::~DirWatcherRegistry() {
    StopAll();
}

// --- id derivation ---

std::string DirWatcherRegistry::MakeId(const std::string& canonical_dir) {
    // SHA-256(canonical_dir), first 8 hex chars (F21 § 2.1). Per-directory key
    // (no namespace component) since one watcher now serves all subscribers.
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx &&
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, canonical_dir.data(), canonical_dir.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1 && digest_len >= 4) {
        EVP_MD_CTX_free(ctx);
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x",
                      digest[0], digest[1], digest[2], digest[3]);
        return std::string(buf);
    }
    if (ctx) EVP_MD_CTX_free(ctx);
    // Defensive fallback (OpenSSL failure is not expected): std::hash.
    unsigned h = static_cast<unsigned>(
        std::hash<std::string>{}(canonical_dir) & 0xFFFFFFFFu);
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", h);
    return std::string(buf);
}

// --- internal helpers (mu_ held) ---

DirWatcherRegistry::WatcherSlot* DirWatcherRegistry::FindSlot(
    const std::string& canonical_dir) {
    for (auto& s : slots_) {
        if (s->canonical_dir == canonical_dir) return s.get();
    }
    return nullptr;
}

const DirWatcherRegistry::WatcherSlot* DirWatcherRegistry::FindSlot(
    const std::string& canonical_dir) const {
    for (const auto& s : slots_) {
        if (s->canonical_dir == canonical_dir) return s.get();
    }
    return nullptr;
}

WatchInfo DirWatcherRegistry::MakeInfo(const WatcherSlot& slot) {
    WatchInfo info;
    info.directory = slot.canonical_dir;
    info.target_namespaces = slot.target_namespaces;
    info.watching = slot.watcher && slot.watcher->IsRunning();
    info.subscriber_count = static_cast<int>(slot.target_namespaces.size());
    info.recursive = slot.recursive;
    info.id = slot.id;
    return info;
}

void DirWatcherRegistry::FanOutEvents(WatcherSlot& slot,
                                      const std::vector<FileEvent>& events) {
    // Filter once at the directory level (fixed ignore_patterns), then fan the
    // surviving events out to every subscribed namespace's importer. Each
    // importer additionally merges/filters internally (idempotent). Events are
    // shared read-only across importers — no per-namespace copy needed.
    FilterConfig fc;
    fc.ignore_patterns = kIgnorePatterns;
    FileFilter filter(fc);

    std::vector<FileEvent> filtered;
    filtered.reserve(events.size());
    for (const auto& ev : events) {
        if (ev.is_directory) continue;
        if (!filter.ShouldImport(ev.path)) continue;
        filtered.push_back(ev);
    }
    if (filtered.empty()) return;

    // rev-deploy M-4 UAF fix: this runs on the OS watcher's event thread
    // (FSEvents dispatch queue / inotify poll loop), concurrently with
    // Subscribe/Unsubscribe mutating slot.importers under mu_. Snapshot the
    // importer shared_ptrs under the lock, then dispatch OUTSIDE the lock — the
    // refcounted snapshot keeps each importer alive even if a concurrent
    // Unsubscribe erases it from the vector mid-batch, and HandleFileEvents
    // (hash + SPC submit, potentially slow) never serializes against, or
    // re-enters under, mu_.
    std::vector<std::shared_ptr<DirectoryImporter>> targets;
    {
        std::lock_guard<std::mutex> lock(mu_);
        targets = slot.importers;  // shared_ptr copies bump the refcount
    }

    for (auto& importer : targets) {
        // Per-NS isolation (§ 5.2.3): a failure in one importer must not stop
        // fan-out to the others. HandleFileEvents swallows per-file errors into
        // its own stats, so a try/catch here is purely defensive.
        try {
            importer->HandleFileEvents(filtered);
        } catch (const std::exception& e) {
            CORTRIX_LOG_WARN(kModule, "fan-out to an importer threw: {}", e.what());
        }
    }
}

Status DirWatcherRegistry::EnsureWatcherStarted(WatcherSlot& slot) {
    if (slot.watcher && slot.watcher->IsRunning()) {
        return Status::Ok();  // already watching
    }
    if (!slot.watcher) {
        slot.watcher = FileWatcher::Create();
    }
    WatcherSlot* slot_ptr = &slot;
    int rc = slot.watcher->Init(
        slot.canonical_dir,
        [this, slot_ptr](const std::vector<FileEvent>& events) {
            FanOutEvents(*slot_ptr, events);
        });
    if (rc != 0) {
        CORTRIX_LOG_ERROR(kModule, "FileWatcher::Init failed rc={} dir={}",
                          rc, slot.canonical_dir);
        slot.watcher.reset();
        return Status::Internal("failed to init file watcher for " +
                                slot.canonical_dir);
    }
    rc = slot.watcher->Start();
    if (rc != 0) {
        CORTRIX_LOG_ERROR(kModule, "FileWatcher::Start failed rc={} dir={}",
                          rc, slot.canonical_dir);
        slot.watcher.reset();
        return Status::Internal("failed to start file watcher for " +
                                slot.canonical_dir);
    }
    CORTRIX_LOG_INFO(kModule, "OS watcher started dir={} subscribers={}",
                     slot.canonical_dir, slot.target_namespaces.size());
    return Status::Ok();
}

// --- subscription management ---

Status DirWatcherRegistry::Subscribe(
    const std::string& directory,
    const std::vector<std::string>& target_namespaces,
    bool recursive,
    bool autostart) {
    if (target_namespaces.empty()) {
        return Status::InvalidArgument("target_namespaces must not be empty");
    }
    std::string canonical_dir = CanonicalDir(directory);
    if (canonical_dir.empty()) {
        return Status::NotFound("directory not found: " + directory);
    }

    std::lock_guard<std::mutex> lock(mu_);

    WatcherSlot* slot = FindSlot(canonical_dir);
    if (!slot) {
        auto new_slot = std::make_unique<WatcherSlot>();
        new_slot->canonical_dir = canonical_dir;
        new_slot->recursive = recursive;
        new_slot->id = MakeId(canonical_dir);
        slot = new_slot.get();
        slots_.push_back(std::move(new_slot));
    }
    // For an existing watcher the recursive flag is fixed (§ 5 edge case: a later
    // Subscribe's recursive arg is silently ignored).

    // Track which importers are newly added so autostart only scans those.
    std::vector<DirectoryImporter*> new_importers;
    for (const auto& ns : target_namespaces) {
        if (ns.empty()) continue;  // skip blank entries defensively
        bool already = std::find(slot->target_namespaces.begin(),
                                 slot->target_namespaces.end(),
                                 ns) != slot->target_namespaces.end();
        if (already) continue;  // idempotent per namespace

        // Auto-create the namespace through the F13 catalog router: this does the
        // catalog INSERT *and* the F05 AdmitCreate (idempotent — AlreadyExists is
        // OK). Admission is required for the importer's facade.Acquire() to resolve
        // below; a bare metadata create does NOT admit into the pool, so the
        // importer would silently skip every file (live-only bug). Mirrors the MVP
        // ConnectorAddWatcher path.
        catalog::NSMetadata meta;
        meta.namespace_id = ns;
        meta.name         = ns;
        Status cs = ins_router_.CreateNamespace(meta);
        if (!cs.ok() && cs.code() != StatusCode::kAlreadyExists) {
            CORTRIX_LOG_WARN(kModule, "namespace create failed ns={}: {}",
                             ns, cs.message());
            // Non-fatal: the importer will surface per-file errors if the NS is
            // genuinely unusable. Continue wiring the subscription.
        }

        auto importer = std::make_shared<DirectoryImporter>(
            MakeImporterConfig(canonical_dir, ns), pool_, spc_mgr_);
        new_importers.push_back(importer.get());
        slot->target_namespaces.push_back(ns);
        slot->importers.push_back(std::move(importer));
    }

    if (autostart) {
        // Initial scan for the newly added importers (an existing watcher's
        // already-subscribed importers were scanned when first added).
        for (auto* imp : new_importers) {
            Status s = imp->Start();
            if (!s.ok()) {
                CORTRIX_LOG_WARN(kModule, "importer initial scan failed dir={}: {}",
                                 canonical_dir, s.message());
            }
        }
        Status ws = EnsureWatcherStarted(*slot);
        if (!ws.ok()) return ws;
    }

    return Status::Ok();
}

Status DirWatcherRegistry::Unsubscribe(const std::string& directory,
                                       const std::string& namespace_id) {
    std::string canonical_dir = CanonicalDir(directory);
    // If the directory no longer resolves, fall back to the raw path so an
    // already-vanished dir can still be unsubscribed by its stored key.
    const std::string& key = canonical_dir.empty() ? directory : canonical_dir;

    std::lock_guard<std::mutex> lock(mu_);

    WatcherSlot* slot = FindSlot(key);
    if (!slot) return Status::NotFound("directory not watched: " + directory);

    auto it = std::find(slot->target_namespaces.begin(),
                        slot->target_namespaces.end(), namespace_id);
    if (it == slot->target_namespaces.end()) {
        return Status::NotFound("namespace not subscribed to directory: " +
                                namespace_id);
    }
    size_t idx = static_cast<size_t>(it - slot->target_namespaces.begin());

    if (idx < slot->importers.size()) {
        slot->importers[idx]->Stop();
        slot->importers.erase(slot->importers.begin() +
                              static_cast<long>(idx));
    }
    slot->target_namespaces.erase(it);

    if (slot->target_namespaces.empty()) {
        if (slot->watcher && slot->watcher->IsRunning()) {
            slot->watcher->Stop();
        }
        for (auto sit = slots_.begin(); sit != slots_.end(); ++sit) {
            if (sit->get() == slot) {
                slots_.erase(sit);
                break;
            }
        }
        CORTRIX_LOG_INFO(kModule, "last subscriber removed, watcher destroyed dir={}",
                         key);
    }
    return Status::Ok();
}

Status DirWatcherRegistry::RemoveWatch(const std::string& directory,
                                       bool delete_docs,
                                       int* out_deleted_docs) {
    std::string canonical_dir = CanonicalDir(directory);
    const std::string& key = canonical_dir.empty() ? directory : canonical_dir;
    if (out_deleted_docs) *out_deleted_docs = 0;

    std::lock_guard<std::mutex> lock(mu_);

    for (auto sit = slots_.begin(); sit != slots_.end(); ++sit) {
        if ((*sit)->canonical_dir != key) continue;
        WatcherSlot* slot = sit->get();
        // Stop the OS watcher FIRST so no fan-out batch races the doc purge /
        // importer teardown below (macOS Stop drains in-flight callbacks; Linux
        // joins the poll thread).
        if (slot->watcher && slot->watcher->IsRunning()) {
            slot->watcher->Stop();
        }
        for (auto& imp : slot->importers) {
            if (delete_docs) {
                int n = imp->DeleteAllDocs();  // cascade: blocks + HNSW + blob
                if (n > 0 && out_deleted_docs) *out_deleted_docs += n;
            }
            imp->Stop();
        }
        slots_.erase(sit);
        CORTRIX_LOG_INFO(kModule, "watcher removed dir={} delete_docs={}",
                         key, delete_docs);
        return Status::Ok();
    }
    return Status::NotFound("directory not watched: " + directory);
}

std::optional<std::string> DirWatcherRegistry::DirectoryForId(
    const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& s : slots_) {
        if (s->id == id) return s->canonical_dir;
    }
    return std::nullopt;
}

// --- queries ---

std::vector<WatchInfo> DirWatcherRegistry::ListWatches() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<WatchInfo> out;
    out.reserve(slots_.size());
    for (const auto& s : slots_) out.push_back(MakeInfo(*s));
    return out;
}

std::optional<WatchInfo> DirWatcherRegistry::GetWatch(
    const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& s : slots_) {
        if (s->id == id) return MakeInfo(*s);
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, ImportStats>>
DirWatcherRegistry::GetStats(const std::string& directory) const {
    std::string canonical_dir = CanonicalDir(directory);
    const std::string& key = canonical_dir.empty() ? directory : canonical_dir;

    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<std::string, ImportStats>> out;
    const WatcherSlot* slot = FindSlot(key);
    if (!slot) return out;
    out.reserve(slot->importers.size());
    // target_namespaces[i] corresponds to importers[i] (kept in lockstep by
    // Subscribe/Unsubscribe).
    for (size_t i = 0; i < slot->importers.size(); ++i) {
        const std::string& ns =
            i < slot->target_namespaces.size() ? slot->target_namespaces[i]
                                               : std::string{};
        out.emplace_back(ns, slot->importers[i]->GetStats());
    }
    return out;
}

ImportStats DirWatcherRegistry::AggregateStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    ImportStats agg;
    for (const auto& s : slots_) {
        for (const auto& imp : s->importers) {
            ImportStats st = imp->GetStats();
            agg.total_files       += st.total_files;
            agg.imported          += st.imported;
            agg.updated           += st.updated;
            agg.skipped_unchanged += st.skipped_unchanged;
            agg.skipped_filtered  += st.skipped_filtered;
            agg.skipped_error     += st.skipped_error;
            agg.deleted           += st.deleted;
            agg.elapsed_s         += st.elapsed_s;
        }
    }
    return agg;
}

// --- control ---

Status DirWatcherRegistry::TriggerScan(const std::string& directory) {
    std::string canonical_dir = CanonicalDir(directory);
    if (canonical_dir.empty()) {
        return Status::NotFound("directory not found: " + directory);
    }
    std::lock_guard<std::mutex> lock(mu_);
    WatcherSlot* slot = FindSlot(canonical_dir);
    if (!slot) return Status::NotFound("directory not watched: " + directory);

    for (auto& imp : slot->importers) {
        Status s = imp->Start();  // Start() == initial (re)scan in F21
        if (!s.ok()) {
            CORTRIX_LOG_WARN(kModule, "TriggerScan importer failed dir={}: {}",
                             canonical_dir, s.message());
        }
    }
    return Status::Ok();
}

void DirWatcherRegistry::StartAll() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& slot : slots_) {
        for (auto& imp : slot->importers) {
            if (!imp->IsWatching()) {
                Status s = imp->Start();
                if (!s.ok()) {
                    CORTRIX_LOG_WARN(kModule, "StartAll importer failed dir={}: {}",
                                     slot->canonical_dir, s.message());
                }
            }
        }
        Status ws = EnsureWatcherStarted(*slot);
        if (!ws.ok()) {
            CORTRIX_LOG_WARN(kModule, "StartAll watcher failed dir={}: {}",
                             slot->canonical_dir, ws.message());
        }
    }
}

void DirWatcherRegistry::StopAll() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& slot : slots_) {
        if (slot->watcher && slot->watcher->IsRunning()) {
            slot->watcher->Stop();
        }
        for (auto& imp : slot->importers) imp->Stop();
    }
}

// --- persistence ---

Status DirWatcherRegistry::SaveState(const std::string& data_dir) const {
    if (data_dir.empty()) {
        return Status::InvalidArgument("data_dir must not be empty");
    }
    namespace fs = std::filesystem;
    fs::path path = fs::path(data_dir) / "watchers.json";

    nlohmann::json j;
    j["version"] = 2;
    j["watchers"] = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& slot : slots_) {
            nlohmann::json w;
            w["directory"] = slot->canonical_dir;
            w["target_namespaces"] = slot->target_namespaces;
            w["recursive"] = slot->recursive;
            j["watchers"].push_back(std::move(w));
        }
    }
    try {
        std::ofstream out(path);
        out << j.dump(2) << "\n";
    } catch (const std::exception& e) {
        return Status::Internal(std::string("failed to write watchers.json: ") +
                                e.what());
    }
    return Status::Ok();
}

Status DirWatcherRegistry::LoadState(const std::string& data_dir) {
    if (data_dir.empty()) {
        return Status::InvalidArgument("data_dir must not be empty");
    }
    namespace fs = std::filesystem;
    fs::path path = fs::path(data_dir) / "watchers.json";

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return Status::Ok();  // first run: nothing to restore
    }

    nlohmann::json j;
    try {
        std::ifstream in(path);
        in >> j;
    } catch (const std::exception& e) {
        // Corrupt file is non-fatal (§ 5 edge case: WARN + skip, do not block startup).
        CORTRIX_LOG_WARN(kModule, "watchers.json parse failed, skipping: {}",
                         e.what());
        return Status::Ok();
    }

    int version = j.value("version", 1);

    // Normalize both layouts into directory -> (recursive, [namespaces]).
    struct Restored {
        bool recursive = true;
        std::vector<std::string> namespaces;
    };
    std::vector<std::string> order;  // preserve first-seen directory order
    std::map<std::string, Restored> by_dir;
    auto add = [&](const std::string& dir, const std::string& ns, bool recursive) {
        auto found = by_dir.find(dir);
        if (found == by_dir.end()) {
            order.push_back(dir);
            Restored r;
            r.recursive = recursive;
            if (!ns.empty()) r.namespaces.push_back(ns);
            by_dir.emplace(dir, std::move(r));
        } else {
            if (!ns.empty() &&
                std::find(found->second.namespaces.begin(),
                          found->second.namespaces.end(),
                          ns) == found->second.namespaces.end()) {
                found->second.namespaces.push_back(ns);
            }
        }
    };

    for (const auto& w : j.value("watchers", nlohmann::json::array())) {
        if (version >= 2) {
            std::string dir = w.value("directory", "");
            if (dir.empty()) continue;
            bool recursive = w.value("recursive", true);
            auto nss = w.value("target_namespaces",
                               std::vector<std::string>{});
            if (nss.empty()) {
                add(dir, "", recursive);  // dir with no NS (degenerate) — skip on subscribe
            }
            for (const auto& ns : nss) add(dir, ns, recursive);
        } else {
            // v1: each entry = (data_dir, namespace_name). Merge same data_dir
            // entries into one watcher's target_namespaces (§ 3.1 / § 4.4).
            std::string dir = w.value("data_dir", "");
            std::string ns = w.value("namespace_name", "default");
            if (dir.empty()) continue;
            add(dir, ns, /*recursive=*/true);
        }
    }

    int restored = 0;
    for (const auto& dir : order) {
        const Restored& r = by_dir[dir];
        if (r.namespaces.empty()) continue;  // nothing to subscribe
        // autostart=false: defer the OS watcher + scan to StartAll().
        Status s = Subscribe(dir, r.namespaces, r.recursive, /*autostart=*/false);
        if (s.ok()) {
            restored++;
        } else {
            CORTRIX_LOG_WARN(kModule, "restore failed dir={}: {}", dir, s.message());
        }
    }
    if (restored > 0) {
        CORTRIX_LOG_INFO(kModule, "{} watcher(s) restored from watchers.json (v{})",
                         restored, version);
    }
    return Status::Ok();
}

}  // namespace cortrix
