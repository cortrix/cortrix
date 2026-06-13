#pragma once
#include "cortrix/async/task_info.h"
#include "cortrix/common/result.h"

namespace cortrix::server {

/// Minimal submission seam the batch service depends on (TD-F42-BULK §3 step 1).
/// Standalone (D3): the batch service is developed + unit-tested against this
/// contract with a mock; the production adapter (F42TaskSubmitterAdapter) forwards
/// to the frozen cortrix::async::TaskScheduler::Enqueue. Keeping the dependency an
/// interface (rather than the concrete TaskScheduler, which has no virtuals) is
/// what lets the per-doc submit fan-out be tested without the F42 worker pool /
/// SQLite tasks table — mirroring how DocumentTaskHandler is unit-tested standalone.
///
/// This is NOT a new public contract: it is a one-method internal adapter over the
/// already-frozen F42 SubmitRequest → Result<TaskInfo> surface. 0 change to F42.
class ITaskSubmitter {
public:
    virtual ~ITaskSubmitter() = default;

    /// Submit one document for async processing. Mirrors F42
    /// TaskScheduler::Enqueue exactly (same SubmitRequest in, Result<TaskInfo>
    /// out): Ok carries the queued/merged task; an error Status surfaces the
    /// per-doc failure (its CX_ERR_* token recoverable from the Status message).
    virtual Result<async::TaskInfo> Submit(const async::SubmitRequest& req) = 0;
};

}  // namespace cortrix::server
