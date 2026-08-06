#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/async/task_info.h"

namespace cortrix::deploy {

/// cortrix_shutdown_status gauge values. Kept in sync with
/// DeployMetrics::SetShutdownStatus.
enum class ShutdownStatus : int {
    kRunning      = 0,
    kShuttingDown = 1,
    kForced       = 2,   ///< grace window elapsed with tasks still pending → persisted
};

/// One pending async task carried across a restart. A minimal subset
/// of async::SubmitRequest — exactly what TaskScheduler::Enqueue needs to re-queue
/// the work after a forced shutdown. Serialized to `.pending_tasks.json`.
struct PendingTask {
    std::string namespace_id;
    std::string filename;
    std::string filepath;
    std::string doc_id;
    std::string content_hash;
    int total_pages = 0;
    int task_type = 1;   ///< async::TaskType value (default doc parse)
};

/// JSON (de)serialization for the `.pending_tasks.json` payload (a top-level
/// {"version":1,"tasks":[...]} object so the schema can evolve). Round-trips
/// PendingTask <-> async::SubmitRequest for the resume path.
nlohmann::json   SerializePending(const std::vector<PendingTask>& tasks);
std::vector<PendingTask> DeserializePending(const nlohmann::json& j);
PendingTask      FromSubmitRequest(const async::SubmitRequest& req);
async::SubmitRequest ToSubmitRequest(const PendingTask& t);

/// The grace-period config the coordinator reads. One IGlobalConfig
/// key with a documented default + range.
struct ShutdownConfig {
    int grace_period_sec = 30;   ///< key "grace_period_sec" (range 5–300)
    static constexpr const char* kGracePeriodKey = "grace_period_sec";
    static constexpr int kReservedSyncSeconds = 7;  ///<: WAL flush + memory persist budget

    std::string data_dir = "./data";   ///< where .pending_tasks.json lives
};

/// Path of the pending-tasks file under `data_dir`. Hidden dotfile.
std::string PendingTasksPath(const std::string& data_dir);

/// Graceful-shutdown coordinator (ordered + user-
/// configurable + persistence-guaranteed + resume). Owns the SIGTERM ordering:
///
///   1. cortrix_shutdown_status = 1; close the HTTP server (stop new requests)
///   2. drain the SPC task queue up to (grace_period_sec - 7s)
///        ├─ drained → clean exit
///        └─ timed out → persist the remaining tasks to .pending_tasks.json,
///                       cortrix_shutdown_status = 2 (forced)
///   3. flush the P-HNSW WAL (fdatasync)
///   4. persist the Memory Store
///
/// Standalone: the *ordering*, the *deadline math*, and the *persist/resume
/// of .pending_tasks.json* are real and fully unit-tested via injected hooks.
/// The hooks (close-http / drain / wal-flush / memory-persist) are bound to the
/// real subsystems by main.cpp at integration wiring time; here they default to no-ops,
/// so the coordinator runs end-to-end against fakes in a test.
///
/// Wires DeployMetrics::SetShutdownStatus on each phase transition.
class GracefulShutdown {
public:
    /// Drain hook: stop accepting + finish in-flight work, returning the tasks
    /// that did NOT complete before `deadline`. An empty return = clean drain.
    using DrainHook = std::function<std::vector<PendingTask>(std::chrono::steady_clock::time_point deadline)>;
    using VoidHook  = std::function<void()>;
    /// Injection seam for the file write (tests capture instead of touching disk).
    /// Default writes atomically under data_dir. Returns false on write failure.
    using PersistHook = std::function<bool(const std::string& path, const std::string& contents)>;

    struct Hooks {
        VoidHook  close_http;       ///< phase 1 — stop the HTTP listener
        DrainHook drain_tasks;      ///< phase 2 — drain SPC queue; returns leftovers
        VoidHook  flush_wal;        ///< phase 3 — P-HNSW WAL flush + fdatasync
        VoidHook  persist_memory;   ///< phase 4 — Memory Store persist
        PersistHook persist_file;   ///< write .pending_tasks.json (null → atomic file write)
    };

    explicit GracefulShutdown(ShutdownConfig config, Hooks hooks = {});

    /// Run the ordered shutdown synchronously (call from the signal-handling
    /// thread, NOT the signal handler itself — see InstallGracefulShutdown).
    /// Returns the final status (kForced if tasks were persisted, else kRunning→
    /// drained clean). Idempotent: a second call is a no-op returning the prior result.
    ShutdownStatus Run();

    /// Startup resume: if .pending_tasks.json exists under data_dir,
    /// parse it, hand each task to `resubmit`, then delete the file. Returns the
    /// number of tasks resumed (0 if the file was absent). A parse error logs +
    /// leaves the file in place (so an operator can inspect it) and returns 0.
    int ResumeOnStartup(const std::function<void(const async::SubmitRequest&)>& resubmit);

    ShutdownStatus status() const { return static_cast<ShutdownStatus>(status_.load()); }
    const ShutdownConfig& config() const { return config_; }

private:
    void SetStatus(ShutdownStatus s);

    ShutdownConfig config_;
    Hooks hooks_;
    std::atomic<int> status_{static_cast<int>(ShutdownStatus::kRunning)};
    std::atomic<bool> ran_{false};
};

/// Default atomic file write (write to `${path}.tmp` then rename) used when
/// Hooks.persist_file is null. Exposed for reuse + tests.
bool AtomicWriteFile(const std::string& path, const std::string& contents);

/// Install `gs` as the process SIGTERM/SIGINT handler (the design's
/// main.cpp entry point). The OS signal handler only sets an atomic flag and
/// notifies; a dedicated waiter thread then runs gs->Run() off the signal
/// context (calling the ordered shutdown — file I/O, joins — from a signal
/// handler is unsafe). `gs` must outlive the process's signal-handling lifetime.
///
/// ⚠️ integration wiring: in the GracefulShutdown's drain/wal/memory hooks are bound
/// to the real SPC pipeline / P-HNSW WAL / Memory Store. Until those hooks are
/// bound (cross-Feature), main.cpp keeps its existing SignalHandler → server.Stop()
/// path and does NOT call this — see the deferred-integration note in main.cpp. The
/// coordinator + this installer are fully implemented and unit-tested standalone.
void InstallGracefulShutdown(GracefulShutdown* gs);

}  // namespace cortrix::deploy
