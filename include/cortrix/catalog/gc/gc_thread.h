#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "cortrix/catalog/gc/document_gc_sweeper.h"
#include "cortrix/catalog/gc/gc_manager.h"

namespace cortrix::catalog::gc {

/// Background GC scan loop (OPEN-2). Wakes every scan_interval_hours and runs both
/// layers each cycle (B① layering): GcManager::RunOnce (catalog.db / blob_gc_queue)
/// then DocumentGcSweeper::RunOnce (per-Unit documents/blocks/vectors/blobs). Same
/// lifecycle shape as async::TaskCleanupCron (thread + cv + stop flag) — "built-in
/// auto GC thread, on par with the P-HNSW background merge" (ARCH).
/// Start()/Stop() are deployment-graceful-shutdown friendly: Stop() signals the cv and
/// joins, so a sweep in flight finishes and the next wait returns immediately.
///
/// The sweeper is optional (nullptr in catalog-only setups / tests). gc.enabled=
/// false → Start() is a no-op (the thread never launches), which is how an operator
/// disables automatic GC while leaving the manual /gc/* ops live.
class GcThread {
public:
    explicit GcThread(GcManager* manager, DocumentGcSweeper* sweeper = nullptr);
    ~GcThread();

    GcThread(const GcThread&) = delete;
    GcThread& operator=(const GcThread&) = delete;

    /// Launch the background loop. No-op if gc.enabled is false or already started.
    void Start();

    /// Signal stop + join the thread. Safe to call twice / without Start().
    void Stop();

    /// TEST-ONLY: wake every `ms` instead of every scan_interval_hours. Set before
    /// Start().
    void set_test_interval_ms(int64_t ms) { test_interval_ms_ = ms; }

    bool started() const { return started_; }

private:
    void RunLoop();
    int64_t IntervalMs() const;

    GcManager* manager_;          // borrowed, must outlive this thread
    DocumentGcSweeper* sweeper_;  // borrowed, optional (may be null)

    std::thread thread_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool started_ = false;
    int64_t test_interval_ms_ = 0;
};

}  // namespace cortrix::catalog::gc
