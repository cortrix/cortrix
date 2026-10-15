#include "cortrix/spc_enricher/enrich_retry_sweeper.h"

#include <chrono>
#include <limits>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/async/task_type.h"
#include "cortrix/async/worker_pool.h"
#include "cortrix/logging/logging.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/spc_enricher/enrich_state_store.h"

namespace cortrix::spc {

namespace {

int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

EnrichRetrySweeper::EnrichRetrySweeper(resource::INamespacePool& pool,
                                       async::TaskScheduler* scheduler,
                                       async::WorkerPool* workers,
                                       const IGlobalConfig* config)
    : pool_(pool), scheduler_(scheduler), workers_(workers), config_(config) {}

EnrichRetrySweeper::~EnrichRetrySweeper() { Stop(); }

int EnrichRetrySweeper::SweepIntervalSec() const {
    if (config_) {
        auto r = config_->GetInt("f42.enrich_sweep_interval_sec");
        if (r.ok() && r.value() > 0) return static_cast<int>(r.value());
    }
    return kDefaultSweepIntervalSec;
}

int EnrichRetrySweeper::MaxDocsPerSweep() const {
    // Same seed path as the interval (yaml spc.enrich_sweep_batch → f42 KV).
    // Absent/non-positive keeps the built-in 32 — byte-identical default.
    if (config_) {
        auto r = config_->GetInt("f42.enrich_sweep_batch");
        if (r.ok() && r.value() > 0) return static_cast<int>(r.value());
    }
    return kMaxDocsPerSweep;
}

void EnrichRetrySweeper::Start() {
    std::lock_guard<std::mutex> lk(cv_mu_);
    if (started_) return;
    started_ = true;
    stop_ = false;
    thread_ = std::thread([this] { RunLoop(); });
}

void EnrichRetrySweeper::Stop() {
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        if (!started_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(cv_mu_);
    started_ = false;
}

void EnrichRetrySweeper::RunLoop() {
    std::unique_lock<std::mutex> lk(cv_mu_);
    while (!stop_) {
        const int64_t wait_ms = test_interval_ms_ > 0
                                    ? test_interval_ms_
                                    : static_cast<int64_t>(SweepIntervalSec()) * 1000;
        cv_.wait_for(lk, std::chrono::milliseconds(wait_ms), [this] { return stop_; });
        if (stop_) break;
        lk.unlock();
        try {
            RunSweepNow();
        } catch (const std::exception& e) {
            CORTRIX_LOG_WARN("spc", "enrich sweep threw, next tick retries: {}", e.what());
        } catch (...) {
            CORTRIX_LOG_WARN("spc", "enrich sweep threw (unknown), next tick retries");
        }
        lk.lock();
    }
}

int EnrichRetrySweeper::RunSweepNow(const std::string& only_ns) {
    // One sweep at a time. The per-doc guard below spans HasActiveTaskFor →
    // LeaseDocRetries → Enqueue while holding no lock across them, so two
    // overlapping sweeps (timer thread vs the backfill route, or two concurrent
    // route calls) could each observe "no active task" for the same doc and each
    // enqueue one. Serializing the body makes that sequence a linearization
    // point for the only producer of kTaskEnrichBackfill.
    std::lock_guard<std::mutex> sweep(sweep_mu_);

    std::vector<std::string> namespaces;
    if (!only_ns.empty()) {
        namespaces.push_back(only_ns);
    } else {
        namespaces = pool_.GetExplainState().active_namespaces;
    }

    const int64_t now = NowUnix();
    const int cap = MaxDocsPerSweep();
    int enqueued = 0;
    for (const auto& ns : namespaces) {
        resource::NamespaceFacade facade(pool_, ns);
        if (!facade.Acquire().ok()) continue;  // unloaded/failed NS → next tick
        sqlite3* db = facade.store().db_handle();
        if (!db) continue;

        // Fetch EVERY due document and let the cap bound how many this tick
        // attempts. A document skipped below keeps its place at the front of the
        // due-ordering — the skip happens before the lease advances next_retry_at —
        // so a batch sized at `cap` would be filled by the same skipped documents
        // on every tick once `cap` of them exist, and nothing behind them would
        // ever be served (#149). The query's cost is in the scan and the GROUP BY,
        // not the LIMIT, so fetching all due documents costs the same.
        auto due = ListDueDocs(db, now, std::numeric_limits<int>::max());
        if (!due.ok()) {
            CORTRIX_LOG_WARN("spc", "enrich sweep ListDueDocs failed ns={}: {}", ns,
                             due.status().message());
            continue;
        }
        int attempted = 0, skipped_active = 0, lookup_failed = 0, lease_failed = 0;
        int created = 0, merged = 0, submit_failed = 0;
        for (const auto& doc_id : due.value()) {
            // The cap bounds attempts (leases taken plus dedup lookups that could
            // not be answered), never the cheap "already has a task" skips — those
            // are exactly the documents that must not consume the batch.
            if (attempted >= cap) break;
            // §3.7.6 enqueue dedup: at most one active backfill task per doc.
            // The 600s lease cannot bound duplicates once queue latency exceeds
            // it (field-observed: 35k+ queued duplicates under a flapping LLM
            // provider), so skip — without consuming the lease — while a
            // queued/processing task for this doc still exists; the doc stays
            // due and the next tick re-checks.
            // The lookup fails closed: an unanswerable dedup check is treated as
            // "an active task may exist", so the doc is skipped with its lease
            // intact rather than leased and re-enqueued. Failing open here would
            // reintroduce the duplicate this guard exists to prevent — a
            // transient task-store read error (SQLITE_BUSY under load is the
            // realistic one) followed by a successful Enqueue is exactly the
            // sequence that produces a second live task for the same doc.
            if (scheduler_) {
                auto active = scheduler_->HasActiveTaskFor(
                    ns, doc_id, async::kTaskEnrichBackfill);
                if (!active.ok()) {
                    CORTRIX_LOG_WARN("spc",
                                     "enrich sweep dedup lookup failed ns={} doc={}: {} - "
                                     "skipping with lease intact, next tick re-checks",
                                     ns, doc_id, active.status().message());
                    ++lookup_failed;
                    ++attempted;  // a failing task store must not be probed without bound
                    continue;
                }
                if (active.value()) {
                    ++skipped_active;
                    continue;
                }
            }
            ++attempted;
            // Lease first: even if Enqueue is debounced/fails, the doc simply
            // waits out the lease instead of hot-looping every tick.
            Status ls = LeaseDocRetries(db, doc_id, now + kLeaseSeconds, now);
            if (!ls.ok()) {
                ++lease_failed;
                continue;
            }
            if (!scheduler_) continue;
            async::SubmitRequest req;
            req.namespace_id = ns;
            req.doc_id = doc_id;
            req.content_hash = doc_id;
            req.filename = "enrich-backfill";
            req.task_type = async::kTaskEnrichBackfill;
            async::EnqueueDisposition disposition = async::EnqueueDisposition::kMerged;
            auto submitted = scheduler_->Enqueue(req, &disposition);
            if (submitted.ok() && disposition == async::EnqueueDisposition::kCreated) {
                ++created;
                ++enqueued;
                continue;
            }
            // Nothing was created for this document: the submission merged into a
            // task that already exists (a just-completed one, under the default
            // debounce), was refreshed, or failed. Counting it would report work
            // that does not exist, and keeping the lease would park the document
            // for 600 s behind a task that will never run for it (#145). Hand the
            // lease back — the same statement with lease_until = now.
            if (submitted.ok()) ++merged; else ++submit_failed;
            Status unlease = LeaseDocRetries(db, doc_id, now, now);
            if (!unlease.ok()) {
                CORTRIX_LOG_WARN("spc", "enrich sweep could not return lease ns={} doc={}: {}",
                                 ns, doc_id, unlease.message());
            }
        }
        if (!due.value().empty()) {
            CORTRIX_LOG_INFO("spc",
                             "enrich sweep ns={} due={} attempted={} skipped_active={} "
                             "lookup_failed={} lease_failed={} created={} merged={} "
                             "submit_failed={}",
                             ns, due.value().size(), attempted, skipped_active,
                             lookup_failed, lease_failed, created, merged, submit_failed);
        }
    }
    if (enqueued > 0 && workers_) workers_->Notify();
    return enqueued;
}

}  // namespace cortrix::spc
