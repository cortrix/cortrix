#include <cstdint>
#include "cortrix/memory/memory_queue.h"

#include <chrono>

#include "cortrix/memory/memory_extract_metrics.h"

namespace cortrix::memory {

MemoryQueue::MemoryQueue() : MemoryQueue(Config{}) {}

MemoryQueue::MemoryQueue(Config config) : config_(config) {
    if (config_.worker_count < 1) config_.worker_count = 1;
    if (config_.queue_max_size == 0) config_.queue_max_size = 1;
    if (config_.retry_max < 0) config_.retry_max = 0;
}

MemoryQueue::~MemoryQueue() { Stop(); }

bool MemoryQueue::Push(const InteractionLog& interaction,
                       const observability::TraceContext* ctx) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.size() >= config_.queue_max_size) {
            return false;  // backpressure — caller falls back to periodic scan
        }
        MemoryQueueItem item;
        item.interaction = interaction;
        if (ctx != nullptr) {
            item.ctx = *ctx;
            item.has_ctx = true;
        }
        queue_.push_back(std::move(item));
        PublishDepthLocked();
    }
    cv_.notify_one();
    return true;
}

size_t MemoryQueue::Depth() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size();
}

void MemoryQueue::PublishDepthLocked() {
    MemoryExtractMetrics::Instance().SetQueueDepth(static_cast<int64_t>(queue_.size()));
}

void MemoryQueue::StartWorkers(MemoryQueueHandler handler,
                               MemoryDeadLetterSink dead_letter) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (workers_started_) return;
        handler_ = std::move(handler);
        dead_letter_ = std::move(dead_letter);
        workers_started_ = true;
        stop_.store(false);
    }
    for (int i = 0; i < config_.worker_count; ++i) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

void MemoryQueue::StartPeriodicScan(MemoryUnextractedSource source) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (periodic_started_) return;
        periodic_source_ = std::move(source);
        periodic_started_ = true;
        stop_.store(false);
    }
    periodic_thread_ = std::thread([this] { PeriodicLoop(); });
}

int MemoryQueue::RunPeriodicScanOnce(const MemoryUnextractedSource& source) {
    if (!source) return 0;
    std::vector<InteractionLog> pending = source();
    int enqueued = 0;
    for (const auto& i : pending) {
        if (Push(i, nullptr)) ++enqueued;
    }
    return enqueued;
}

bool MemoryQueue::Pop(MemoryQueueItem* out) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return stop_.load() || !queue_.empty(); });
    if (queue_.empty()) {
        return false;  // woken to stop, nothing left
    }
    *out = std::move(queue_.front());
    queue_.pop_front();
    PublishDepthLocked();
    return true;
}

void MemoryQueue::WorkerLoop() {
    for (;;) {
        MemoryQueueItem item;
        if (!Pop(&item)) {
            return;  // stopping + drained
        }
        bool ok = false;
        if (handler_) {
            ok = handler_(item);
        }
        if (ok) {
            processed_count_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // Failed → retry up to retry_max, else dead-letter.
        if (item.attempt < config_.retry_max) {
            item.attempt += 1;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!stop_.load() && queue_.size() < config_.queue_max_size) {
                    queue_.push_back(std::move(item));
                    PublishDepthLocked();
                    cv_.notify_one();
                    continue;
                }
            }
            // Queue full or stopping during retry → fall through to dead-letter so
            // the item is not silently dropped.
        }
        dead_letter_count_.fetch_add(1, std::memory_order_relaxed);
        if (dead_letter_) dead_letter_(item);
    }
}

void MemoryQueue::PeriodicLoop() {
    using namespace std::chrono;
    // Sleep in short slices so Stop() is responsive even with a long interval.
    const auto interval = seconds(config_.periodic_scan_interval_s);
    auto next = steady_clock::now() + interval;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            // Wait until the next tick or stop, whichever first.
            cv_.wait_until(lk, next, [this] { return stop_.load(); });
            if (stop_.load()) return;
        }
        if (steady_clock::now() >= next) {
            MemoryUnextractedSource src;
            {
                std::lock_guard<std::mutex> lk(mu_);
                src = periodic_source_;
            }
            RunPeriodicScanOnce(src);
            next = steady_clock::now() + interval;
        }
    }
}

void MemoryQueue::Stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_.load()) {
            // Already stopping/stopped; still join below in case threads linger.
        }
        stop_.store(true);
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    if (periodic_thread_.joinable()) periodic_thread_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        workers_started_ = false;
        periodic_started_ = false;
    }
}

int MemoryQueue::worker_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(workers_.size());
}

}  // namespace cortrix::memory
