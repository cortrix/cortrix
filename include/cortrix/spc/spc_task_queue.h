#pragma once
#include <vector>
#include <mutex>
#include <condition_variable>
#include <memory>
#include "cortrix/spc/spc_task.h"

namespace cortrix {

/// Priority queue for SPC tasks with cancellation support
class SPCTaskQueue {
public:
    explicit SPCTaskQueue(int max_size = 10000);

    /// Push task into queue (priority ordered)
    /// @return 0 = success, -1 = queue full
    int Push(std::shared_ptr<SPCTask> task);

    /// Pop highest priority task (blocks until available or stopped)
    /// @return nullptr if queue stopped
    std::shared_ptr<SPCTask> Pop();

    /// Cancel the queued tasks whose namespace_name, source_type and source_path
    /// all match; returns the number cancelled.
    int CancelQueuedVersions(const std::string& namespace_name,
                             const std::string& source_type,
                             const std::string& source_path);

    /// True while a task for doc_id waits in the queue and has not been cancelled.
    /// A task a worker has already popped is not queued.
    bool HasQueuedTask(const std::string& doc_id) const;

    /// Stop the queue (unblock all waiting Pop calls)
    void Stop();

    /// [D3.5 gap⑤ · F24 §7.2.bis] Move out every not-yet-started task (graceful-
    /// shutdown drain). Cancelled tasks are skipped. Call AFTER Stop() + worker
    /// join (the manager guarantees no concurrent Pop); the queue is left empty.
    std::vector<std::shared_ptr<SPCTask>> DrainRemaining();

    /// Current queue size
    size_t Size() const;

    bool IsStopped() const;

private:
    int max_size_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<SPCTask>> queue_;
    bool stopped_ = false;
};

}  // namespace cortrix
