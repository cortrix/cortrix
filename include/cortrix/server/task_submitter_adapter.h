#pragma once
#include "cortrix/async/task_scheduler.h"
#include "cortrix/server/i_task_submitter.h"

namespace cortrix::server {

/// Production ITaskSubmitter — forwards each per-doc submission to the frozen
/// scheduler (cortrix::async::TaskScheduler::Enqueue), then wakes a worker. This
/// is the single binding point between the batch service and the scheduler; swapping it is
/// the only change needed if the per-doc submission seam is ever re-targeted.
/// 0 change to the scheduler: it only *calls* the already-frozen Enqueue surface.
///
/// integration: the SubmitRequest handed here must already carry a server-side
/// `filepath` (the materialized inline content) — that materialization is the
/// batch service's integration deferred step (see BatchSubmitService). Standalone, the
/// service is exercised against MockTaskSubmitter, not this adapter.
class TaskSubmitterAdapter : public ITaskSubmitter {
public:
    /// @param scheduler borrowed frozen task scheduler (must outlive this adapter)
    /// @param pool      borrowed worker pool to Notify() after a successful
    ///                  enqueue (nullptr ok — no worker wake, e.g. in tests)
    explicit TaskSubmitterAdapter(async::TaskScheduler* scheduler,
                                     async::WorkerPool* pool = nullptr)
        : scheduler_(scheduler), pool_(pool) {}

    Result<async::TaskInfo> Submit(const async::SubmitRequest& req) override {
        auto enq = scheduler_->Enqueue(req);
        if (enq.ok() && pool_) pool_->Notify();
        return enq;
    }

private:
    async::TaskScheduler* scheduler_;
    async::WorkerPool* pool_;
};

}  // namespace cortrix::server
