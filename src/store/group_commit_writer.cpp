#include <cstdint>
#include "cortrix/store/group_commit_writer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iterator>
#include <utility>

#include <spdlog/spdlog.h>

namespace cortrix::store {

namespace {

// IEEE 802.3 CRC32 lookup table, built once on first use.
const std::array<std::uint32_t, 256>& Crc32Table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

}  // namespace

GroupCommitWriter::GroupCommitWriter(IWalSink* sink, int max_batch, int timeout_ms)
    : sink_(sink),
      max_batch_(max_batch < 1 ? 1u : static_cast<std::size_t>(max_batch)),
      timeout_ms_(timeout_ms < 0 ? 0 : timeout_ms) {
    worker_ = std::thread(&GroupCommitWriter::FlushLoop, this);
}

GroupCommitWriter::~GroupCommitWriter() {
    Shutdown();
}

std::future<Status> GroupCommitWriter::Submit(std::vector<uint8_t> entry_bytes) {
    Pending p;
    p.bytes = std::move(entry_bytes);
    std::future<Status> fut = p.done.get_future();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) {
            // Resolve immediately; do not enqueue work that will never flush.
            p.done.set_value(Status::Unavailable("GroupCommitWriter is shut down"));
            return fut;
        }
        ++stats_.submitted;
        queue_.push_back(std::move(p));
    }
    cv_.notify_one();
    return fut;
}

void GroupCommitWriter::Shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;
        shutdown_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();  // FlushLoop drains the queue before exiting
    }
}

GroupCommitStats GroupCommitWriter::GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    GroupCommitStats snapshot = stats_;
    snapshot.pending = queue_.size();
    return snapshot;
}

void GroupCommitWriter::FlushLoop() {
    for (;;) {
        std::vector<Pending> batch;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return !queue_.empty() || shutdown_; });
            if (queue_.empty() && shutdown_) {
                return;  // fully drained and asked to stop
            }
            // Coalesce: if the batch is not yet full and we are not shutting
            // down, wait up to timeout_ms for more entries to pile up.
            if (queue_.size() < max_batch_ && !shutdown_) {
                cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms_),
                             [this] { return queue_.size() >= max_batch_ || shutdown_; });
            }
            const std::size_t take = std::min(queue_.size(), max_batch_);
            batch.insert(batch.end(),
                         std::make_move_iterator(queue_.begin()),
                         std::make_move_iterator(queue_.begin() + take));
            queue_.erase(queue_.begin(), queue_.begin() + take);
        }
        // Slow path (WriteBatch + Sync) runs without the lock so concurrent
        // Submit() calls are never blocked on I/O — the point of group commit.
        //
        // R2-M3 — defensive catch-all around the whole flush so nothing escapes
        // this background thread's entry point (uncaught → std::terminate, whole-
        // process crash). FlushBatch already maps a throwing sink to an error
        // Status internally; this outer guard covers any other throw (e.g. a
        // promise resolution) so the flush thread keeps draining. On such a throw
        // we still resolve every waiter in this batch — set_value on an
        // already-satisfied promise throws future_error, which we ignore per
        // waiter — so no Submit() caller is left blocked forever.
        if (!batch.empty()) {
            try {
                FlushBatch(batch);
            } catch (const std::exception& e) {
                spdlog::error("GroupCommitWriter: unexpected exception flushing {} "
                              "entries: {} — resolving waiters, loop continues",
                              batch.size(), e.what());
                ResolvePendingWaiters(batch);
            } catch (...) {
                spdlog::error("GroupCommitWriter: unexpected non-std exception "
                              "flushing {} entries — resolving waiters, loop continues",
                              batch.size());
                ResolvePendingWaiters(batch);
            }
        }
    }
}

void GroupCommitWriter::ResolvePendingWaiters(std::vector<Pending>& batch) {
    // Last-resort resolution after an unexpected flush exception: ensure no
    // Submit() future is left hanging. A promise that FlushBatch already resolved
    // throws future_error on a second set_value — swallow it per waiter.
    for (auto& p : batch) {
        try {
            p.done.set_value(
                Status::Unavailable("GroupCommitWriter flush failed unexpectedly"));
        } catch (...) {
            // Already satisfied by FlushBatch — nothing to do.
        }
    }
}

void GroupCommitWriter::FlushBatch(std::vector<Pending>& batch) {
    std::vector<std::vector<uint8_t>> entries;
    entries.reserve(batch.size());
    for (auto& p : batch) {
        entries.push_back(std::move(p.bytes));
    }

    // R2-M3 — a throwing sink must not escape the flush thread (uncaught →
    // std::terminate, whole-process crash). Treat any exception from WriteBatch/
    // Sync as a non-durable batch: map it to an Unavailable Status and fall
    // through to resolve every waiter below. This preserves the contract that
    // every Submit() future is resolved (callers see a clean error Status, never
    // a broken_promise future_error) and that an Ok future implies a durable
    // Sync().
    Status st;
    try {
        st = sink_->WriteBatch(entries);
        if (st.ok()) {
            st = sink_->Sync();
        }
    } catch (const std::exception& e) {
        spdlog::error("GroupCommitWriter: sink threw during flush of {} entries: {} "
                      "— batch reported not durable",
                      batch.size(), e.what());
        st = Status::Unavailable("GroupCommitWriter sink threw during flush");
    } catch (...) {
        spdlog::error("GroupCommitWriter: sink threw a non-std exception during "
                      "flush of {} entries — batch reported not durable",
                      batch.size());
        st = Status::Unavailable("GroupCommitWriter sink threw during flush");
    }

    // Resolve every waiter with the batch's durability status. A future only
    // observes Ok after Sync() returned Ok, so an Ok future == crash-safe.
    for (auto& p : batch) {
        p.done.set_value(st);
    }

    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.batches;
    if (st.ok()) {
        stats_.synced += batch.size();
    } else {
        stats_.failed += batch.size();
    }
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t length) {
    const std::array<std::uint32_t, 256>& table = Crc32Table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace cortrix::store
