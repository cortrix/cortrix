#include "cortrix/deploy/graceful_shutdown.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "cortrix/deploy/deploy_metrics.h"

namespace cortrix::deploy {

// --------------------------- serialization ---------------------------

nlohmann::json SerializePending(const std::vector<PendingTask>& tasks) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tasks) {
        arr.push_back({
            {"namespace_id", t.namespace_id},
            {"filename", t.filename},
            {"filepath", t.filepath},
            {"doc_id", t.doc_id},
            {"content_hash", t.content_hash},
            {"total_pages", t.total_pages},
            {"task_type", t.task_type},
        });
    }
    return nlohmann::json{{"version", 1}, {"tasks", std::move(arr)}};
}

std::vector<PendingTask> DeserializePending(const nlohmann::json& j) {
    std::vector<PendingTask> out;
    if (!j.is_object() || !j.contains("tasks") || !j["tasks"].is_array()) {
        return out;
    }
    for (const auto& e : j["tasks"]) {
        if (!e.is_object()) continue;
        PendingTask t;
        t.namespace_id = e.value("namespace_id", "");
        t.filename     = e.value("filename", "");
        t.filepath     = e.value("filepath", "");
        t.doc_id       = e.value("doc_id", "");
        t.content_hash = e.value("content_hash", "");
        t.total_pages  = e.value("total_pages", 0);
        t.task_type    = e.value("task_type", 1);
        out.push_back(std::move(t));
    }
    return out;
}

PendingTask FromSubmitRequest(const async::SubmitRequest& req) {
    PendingTask t;
    t.namespace_id = req.namespace_id;
    t.filename     = req.filename;
    t.filepath     = req.filepath;
    t.doc_id       = req.doc_id;
    t.content_hash = req.content_hash;
    t.total_pages  = req.total_pages;
    t.task_type    = req.task_type;
    return t;
}

async::SubmitRequest ToSubmitRequest(const PendingTask& t) {
    async::SubmitRequest req;
    req.namespace_id = t.namespace_id;
    req.filename     = t.filename;
    req.filepath     = t.filepath;
    req.doc_id       = t.doc_id;
    req.content_hash = t.content_hash;
    req.total_pages  = t.total_pages;
    req.task_type    = t.task_type;
    return req;
}

std::string PendingTasksPath(const std::string& data_dir) {
    std::string d = data_dir;
    if (!d.empty() && d.back() == '/') d.pop_back();
    return d + "/.pending_tasks.json";
}

bool AtomicWriteFile(const std::string& path, const std::string& contents) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << contents;
        out.flush();
        if (!out) return false;
    }
    // rename is atomic on the same filesystem (POSIX); replaces any prior file.
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// --------------------------- coordinator ---------------------------

GracefulShutdown::GracefulShutdown(ShutdownConfig config, Hooks hooks)
    : config_(std::move(config)), hooks_(std::move(hooks)) {}

void GracefulShutdown::SetStatus(ShutdownStatus s) {
    status_.store(static_cast<int>(s));
    DeployMetrics::Instance().SetShutdownStatus(static_cast<int>(s));
}

ShutdownStatus GracefulShutdown::Run() {
    bool expected = false;
    if (!ran_.compare_exchange_strong(expected, true)) {
        return status();  // idempotent
    }

    // Phase 1: announce shutting_down + stop accepting new requests.
    SetStatus(ShutdownStatus::kShuttingDown);
    if (hooks_.close_http) hooks_.close_http();

    // Phase 2: drain the SPC queue up to (grace - reserved sync budget).
    int grace = config_.grace_period_sec;
    int drain_budget = grace - ShutdownConfig::kReservedSyncSeconds;
    if (drain_budget < 0) drain_budget = 0;  // tiny grace → no drain window
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(drain_budget);

    std::vector<PendingTask> pending;
    if (hooks_.drain_tasks) pending = hooks_.drain_tasks(deadline);

    if (!pending.empty()) {
        // Forced: persist leftovers so the next start can resume them.
        const std::string path = PendingTasksPath(config_.data_dir);
        const std::string body = SerializePending(pending).dump(2);
        bool ok = hooks_.persist_file ? hooks_.persist_file(path, body)
                                      : AtomicWriteFile(path, body);
        (void)ok;  // a failed persist still proceeds to exit; status reflects forced
        SetStatus(ShutdownStatus::kForced);
    }

    // Phase 3: P-HNSW WAL flush + fdatasync. Phase 4: Memory Store persist.
    if (hooks_.flush_wal) hooks_.flush_wal();
    if (hooks_.persist_memory) hooks_.persist_memory();

    return status();
}

int GracefulShutdown::ResumeOnStartup(
    const std::function<void(const async::SubmitRequest&)>& resubmit) {
    const std::string path = PendingTasksPath(config_.data_dir);
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;  // no pending file → nothing to resume

    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ss.str());
    } catch (...) {
        // Leave the file in place for inspection; do not crash the boot path.
        return 0;
    }

    std::vector<PendingTask> tasks = DeserializePending(j);
    if (resubmit) {
        for (const auto& t : tasks) resubmit(ToSubmitRequest(t));
    }
    std::remove(path.c_str());  // consumed → delete
    return static_cast<int>(tasks.size());
}

// --------------------------- signal installer ---------------------------

namespace {

// Signal-handler-safe rendezvous: the handler only flips an atomic + notifies via
// a self-pipe-free flag the waiter thread polls. We keep a single global target
// (the process has one shutdown coordinator).
std::atomic<GracefulShutdown*> g_shutdown_target{nullptr};
std::atomic<bool> g_signal_received{false};

void SignalTrampoline(int /*sig*/) {
    // async-signal-safe: only set an atomic flag. No file I/O / locks here.
    g_signal_received.store(true, std::memory_order_relaxed);
}

}  // namespace

void InstallGracefulShutdown(GracefulShutdown* gs) {
    if (!gs) return;
    g_shutdown_target.store(gs, std::memory_order_relaxed);
    g_signal_received.store(false, std::memory_order_relaxed);

    std::signal(SIGTERM, SignalTrampoline);
    std::signal(SIGINT, SignalTrampoline);

    // Detached waiter: wakes on the flag and runs the ordered shutdown off the
    // signal context. Lightweight poll (the process is exiting once it fires).
    std::thread([] {
        while (!g_signal_received.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (auto* gs = g_shutdown_target.load(std::memory_order_relaxed)) {
            gs->Run();
        }
    }).detach();
}

}  // namespace cortrix::deploy
