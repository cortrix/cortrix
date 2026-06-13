#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cortrix/memory/interaction_log.h"
#include "cortrix/observability/trace_context.h"

// MEM02 async extraction queue (D2 decision: async queue main line + periodic
// fallback). A self-contained C++ in-process queue + resident worker pool that
// drains queued interactions through a supplied handler.
//
// 🚨 D3 standalone discipline (B_R3_BRIEFING §2 redline 2): this is modeled on the
// F42 WorkerPool fan-out *pattern* but does NOT #include or consume any F42 code —
// it is an independent queue scoped to MEM02. The real cross-language async worker
// (the Python middleware in MEM02 §3.3.1) and the real cortrix_log_interaction
// trigger are DEFERRED → D3.5; this kernel-side queue is the in-process basis they
// build on and is fully testable standalone (push → worker → handler).
//
// The TraceContext (MEM02 §5.5.2) travels with each queued item so the async link
// keeps the W3C trace_id (the worker constructs its span with parent = ctx.span_id).
namespace cortrix::memory {

/// One queued extraction job: the interaction to extract plus its propagated trace.
struct MemoryQueueItem {
    InteractionLog interaction;
    observability::TraceContext ctx;   ///< W3C trace carried across the async boundary
    bool has_ctx = false;              ///< whether ctx is populated
    int attempt = 0;                   ///< retry counter (0 = first try)
};

/// Worker handler: process one item. Return true on success; false routes the item
/// to retry (until retry_max) then to the dead-letter sink. The MemoryExtractor's
/// Extract() is the production handler (wired by the owner); standalone tests inject
/// a lambda.
using MemoryQueueHandler = std::function<bool(const MemoryQueueItem&)>;

/// Dead-letter sink: invoked when an item exhausts its retries (D2 reliability).
using MemoryDeadLetterSink = std::function<void(const MemoryQueueItem&)>;

/// Periodic-scan source: returns interactions that were never extracted (the D2
/// hourly fallback re-enqueues these). Standalone tests inject a lambda; the real
/// source (a store query for unextracted interactions) is wired in D3.5.
using MemoryUnextractedSource = std::function<std::vector<InteractionLog>()>;

class MemoryQueue {
public:
    /// Worker-pool + reliability config (MEM02 §3.3 Config + §4.3 mem02.queue).
    struct Config {
        int worker_count = 4;                 ///< resident worker threads (NS-level configurable)
        size_t queue_max_size = 10000;        ///< backpressure cap; push past it is rejected
        int retry_max = 3;                    ///< per-item retries before dead-letter
        int periodic_scan_interval_s = 3600;  ///< D2 fallback sweep interval
    };

    MemoryQueue();                       ///< default Config
    explicit MemoryQueue(Config config);
    ~MemoryQueue();

    MemoryQueue(const MemoryQueue&) = delete;
    MemoryQueue& operator=(const MemoryQueue&) = delete;

    /// Enqueue an interaction for async extraction (the cortrix_log_interaction
    /// trigger calls this; D3.5 wiring). Returns false if the queue is at
    /// queue_max_size (backpressure) — the caller may fall back to the periodic scan.
    bool Push(const InteractionLog& interaction,
              const observability::TraceContext* ctx = nullptr);

    /// Current queue depth (also mirrored to cortrix_mem02_queue_depth on push/pop).
    size_t Depth() const;

    /// Start `worker_count` worker threads draining the queue through `handler`.
    /// `dead_letter` (optional) receives items that exhaust retries. No-op if already
    /// started.
    void StartWorkers(MemoryQueueHandler handler,
                      MemoryDeadLetterSink dead_letter = nullptr);

    /// Start the D2 periodic fallback: every periodic_scan_interval_s, call `source`
    /// and re-enqueue any returned (unextracted) interactions. No-op if already started.
    void StartPeriodicScan(MemoryUnextractedSource source);

    /// Run one periodic scan immediately (test hook + the worker's first-tick path):
    /// pull from `source` and enqueue. Returns the number enqueued.
    int RunPeriodicScanOnce(const MemoryUnextractedSource& source);

    /// Stop + join all workers and the periodic-scan thread. Drains nothing new;
    /// in-flight handler calls run to completion. Idempotent; also run by the dtor.
    void Stop();

    /// Number of resident workers (post-StartWorkers; 0 before).
    int worker_count() const;

    /// Total items that exhausted retries and hit the dead-letter sink (test/metric aid).
    uint64_t dead_letter_count() const { return dead_letter_count_.load(); }

    /// Total items successfully processed (test/metric aid).
    uint64_t processed_count() const { return processed_count_.load(); }

private:
    void WorkerLoop();
    void PeriodicLoop();
    /// Pop one item (blocking until available or stop). Returns false when stopping
    /// and the queue is drained.
    bool Pop(MemoryQueueItem* out);
    void PublishDepthLocked();  // mirror size to the metric; caller holds mu_

    Config config_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<MemoryQueueItem> queue_;
    std::atomic<bool> stop_{false};

    std::vector<std::thread> workers_;
    MemoryQueueHandler handler_;
    MemoryDeadLetterSink dead_letter_;
    bool workers_started_ = false;

    std::thread periodic_thread_;
    MemoryUnextractedSource periodic_source_;
    bool periodic_started_ = false;

    std::atomic<uint64_t> dead_letter_count_{0};
    std::atomic<uint64_t> processed_count_{0};
};

}  // namespace cortrix::memory
