#include "cortrix/spc_enricher/enrich_retry_sweeper.h"

#include <chrono>

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
        auto r = config_->GetInt("async.enrich_sweep_interval_sec");
        if (r.ok() && r.value() > 0) return static_cast<int>(r.value());
    }
    return kDefaultSweepIntervalSec;
}

int EnrichRetrySweeper::MaxDocsPerSweep() const {
    // Same seed path as the interval (yaml spc.enrich_sweep_batch → async KV).
    // Absent/non-positive keeps the built-in 32 — byte-identical default.
    if (config_) {
        auto r = config_->GetInt("async.enrich_sweep_batch");
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
    std::vector<std::string> namespaces;
    if (!only_ns.empty()) {
        namespaces.push_back(only_ns);
    } else {
        namespaces = pool_.GetExplainState().active_namespaces;
    }

    const int64_t now = NowUnix();
    int enqueued = 0;
    for (const auto& ns : namespaces) {
        resource::NamespaceFacade facade(pool_, ns);
        if (!facade.Acquire().ok()) continue;  // unloaded/failed NS → next tick
        sqlite3* db = facade.store().db_handle();
        if (!db) continue;

        auto due = ListDueDocs(db, now, MaxDocsPerSweep());
        if (!due.ok()) {
            CORTRIX_LOG_WARN("spc", "enrich sweep ListDueDocs failed ns={}: {}", ns,
                             due.status().message());
            continue;
        }
        for (const auto& doc_id : due.value()) {
            // Lease first: even if Enqueue is debounced/fails, the doc simply
            // waits out the lease instead of hot-looping every tick.
            Status ls = LeaseDocRetries(db, doc_id, now + kLeaseSeconds, now);
            if (!ls.ok()) continue;
            async::SubmitRequest req;
            req.namespace_id = ns;
            req.doc_id = doc_id;
            req.content_hash = doc_id;
            req.filename = "enrich-backfill";
            req.task_type = async::kTaskEnrichBackfill;
            if (scheduler_ && scheduler_->Enqueue(req).ok()) ++enqueued;
        }
    }
    if (enqueued > 0) {
        CORTRIX_LOG_INFO("spc", "enrich sweep enqueued {} backfill task(s)", enqueued);
        if (workers_) workers_->Notify();
    }
    return enqueued;
}

}  // namespace cortrix::spc
