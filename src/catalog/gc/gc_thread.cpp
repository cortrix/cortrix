#include <cstdint>
#include "cortrix/catalog/gc/gc_thread.h"

#include <chrono>

#include <spdlog/spdlog.h>

namespace cortrix::catalog::gc {

GcThread::GcThread(GcManager* manager, DocumentGcSweeper* sweeper)
    : manager_(manager), sweeper_(sweeper) {}

GcThread::~GcThread() { Stop(); }

int64_t GcThread::IntervalMs() const {
    if (test_interval_ms_ > 0) return test_interval_ms_;
    return static_cast<int64_t>(manager_->config().scan_interval_hours) * 3600 * 1000;
}

void GcThread::Start() {
    std::lock_guard<std::mutex> lock(cv_mu_);
    if (started_) return;
    // gc.enabled=false → the automatic thread never launches; manual /gc/* still work.
    if (!manager_->config().enabled) {
        spdlog::info("GC background thread disabled (gc.enabled=false)");
        return;
    }
    started_ = true;
    stop_ = false;
    thread_ = std::thread([this] { RunLoop(); });
}

void GcThread::Stop() {
    {
        std::lock_guard<std::mutex> lock(cv_mu_);
        if (!started_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    started_ = false;
}

void GcThread::RunLoop() {
    std::unique_lock<std::mutex> lock(cv_mu_);
    while (!stop_) {
        cv_.wait_for(lock, std::chrono::milliseconds(IntervalMs()),
                     [this] { return stop_; });
        if (stop_) break;
        // Release the cv lock while sweeping (each layer takes its own lock).
        lock.unlock();
        // Layer 1: catalog.db / blob_gc_queue (file_locations content-addressed GC).
        auto r = manager_->RunOnce();
        if (r.ok()) {
            const auto& rep = r.value();
            if (rep.hard_deleted > 0 || rep.blobs_unlinked > 0) {
                spdlog::info("GC sweep: {} files hard-deleted, {} blobs unlinked{}",
                             rep.hard_deleted, rep.blobs_unlinked,
                             rep.hit_run_cap ? " (run cap hit)" : "");
            }
        } else {
            spdlog::warn("GC sweep failed: {}", r.status().message());
        }
        // Layer 2: per-Unit document GC (Stage 2 hard delete of expired soft-deletes).
        if (sweeper_ != nullptr) {
            auto d = sweeper_->RunOnce();
            if (d.ok()) {
                const auto& rep = d.value();
                if (rep.docs_hard_deleted > 0) {
                    spdlog::info("Doc GC sweep: {} docs hard-deleted ({} blocks, {} blobs){}",
                                 rep.docs_hard_deleted, rep.blocks_deleted, rep.blobs_deleted,
                                 rep.hit_run_cap ? " (run cap hit)" : "");
                }
            } else {
                spdlog::warn("Doc GC sweep failed: {}", d.status().message());
            }
        }
        lock.lock();
    }
}

}  // namespace cortrix::catalog::gc
