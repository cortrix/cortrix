#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cortrix/common/i_global_config.h"

namespace cortrix {

namespace async {
class TaskScheduler;
class WorkerPool;
}  // namespace async
namespace resource { class INamespacePool; }

namespace spc {

/// EnrichRetrySweeper — the addendum §3.7 periodic sentinel that turns durable
/// enrich_state debt into kTaskEnrichBackfill work. Every sweep tick it walks
/// the loaded namespaces, finds documents with due 'pending_retry' rows
/// (next_retry_at <= now), LEASES them forward (so the next tick cannot
/// re-enqueue a doc whose task is still queued/running) and enqueues one
/// backfill task per doc through the F42 TaskScheduler (+ WorkerPool::Notify).
///
/// enrich_state itself is the persistent queue SoT — the sweeper holds no state,
/// so crashes lose nothing (the lease simply expires and the doc is re-swept).
/// Thread shape mirrors async::TaskCleanupCron (thread + cv + stop flag; the
/// sweep body is exposed as RunSweepNow for tests and for the backfill ops API).
class EnrichRetrySweeper {
public:
    /// @param pool      borrowed F05 pool (namespace enumeration + per-NS acquire).
    /// @param scheduler borrowed F42 scheduler (Enqueue; debounces by doc_id).
    /// @param workers   borrowed F42 worker pool (Notify after enqueues); may be
    ///                  nullptr in tests (enqueue still happens, no wake).
    /// @param config    borrowed IGlobalConfig for f42.enrich_sweep_interval_sec
    ///                  (nullptr → default).
    EnrichRetrySweeper(resource::INamespacePool& pool,
                       async::TaskScheduler* scheduler,
                       async::WorkerPool* workers,
                       const IGlobalConfig* config);
    ~EnrichRetrySweeper();

    EnrichRetrySweeper(const EnrichRetrySweeper&) = delete;
    EnrichRetrySweeper& operator=(const EnrichRetrySweeper&) = delete;

    void Start();
    void Stop();

    /// One synchronous sweep over all loaded namespaces (or just `only_ns` when
    /// non-empty). Returns the number of backfill tasks enqueued.
    int RunSweepNow(const std::string& only_ns = "");

    static constexpr int kDefaultSweepIntervalSec = 60;
    /// Lease horizon: a swept doc's rows are pushed this far into the future; the
    /// worker overwrites the real next_retry_at when it finishes. Must exceed the
    /// worst-case queue wait + per-doc repair time.
    static constexpr int kLeaseSeconds = 600;
    /// Per-NS per-sweep doc cap (bounds one tick's enqueue burst).
    static constexpr int kMaxDocsPerSweep = 32;

    /// TEST-ONLY: wake every `ms` instead of the configured interval.
    void set_test_interval_ms(int64_t ms) { test_interval_ms_ = ms; }

    /// Effective sweep interval: `f42.enrich_sweep_interval_sec` KV when seeded
    /// (yaml spc.enrich_sweep_interval_sec), else kDefaultSweepIntervalSec.
    /// Public so startup logging can report the value actually in force —
    /// the old log printed the compile-time constant and masked live knob
    /// state twice (2026-07-10/11, D7b).
    int SweepIntervalSec() const;

private:
    void RunLoop();
    /// Per-NS per-tick doc cap: `f42.enrich_sweep_batch` KV when seeded (yaml
    /// spc.enrich_sweep_batch), else the built-in kMaxDocsPerSweep.
    int MaxDocsPerSweep() const;

    resource::INamespacePool& pool_;
    async::TaskScheduler* scheduler_;
    async::WorkerPool* workers_;
    const IGlobalConfig* config_;

    std::thread thread_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool started_ = false;
    int64_t test_interval_ms_ = 0;

    /// Serializes the sweep body. RunSweepNow has two production callers — the
    /// timer thread and the backfill ops route — and the dedup guard inside it
    /// spans a check, a lease and an enqueue with no lock held across them, so
    /// overlapping sweeps could each see "no active task" and each enqueue.
    /// Held for the whole body rather than try_lock'd: a missed try_lock could
    /// only report zero enqueued, which the route cannot distinguish from
    /// "nothing was due" — and that route exists precisely to force a backfill.
    /// Distinct from cv_mu_, which RunLoop holds across its wait and releases
    /// before calling RunSweepNow, so Stop() cannot deadlock against a sweep.
    std::mutex sweep_mu_;
};

}  // namespace spc
}  // namespace cortrix
