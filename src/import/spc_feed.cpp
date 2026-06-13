#include "cortrix/import/spc_feed.h"

namespace cortrix::import {

// --- InMemorySpcFeeder ---

Result<int> InMemorySpcFeeder::Feed(const std::vector<TextChunk>& chunks, const NsId& ns) {
    std::lock_guard<std::mutex> lock(mu_);
    batches_.push_back(FedBatch{ns, chunks});
    return static_cast<int>(chunks.size());
}

std::vector<InMemorySpcFeeder::FedBatch> InMemorySpcFeeder::batches() const {
    std::lock_guard<std::mutex> lock(mu_);
    return batches_;
}

int InMemorySpcFeeder::total_chunks() const {
    std::lock_guard<std::mutex> lock(mu_);
    int n = 0;
    for (const auto& b : batches_) n += static_cast<int>(b.chunks.size());
    return n;
}

// --- InMemoryBlockCleaner ---

void InMemoryBlockCleaner::SeedBlock(const NsId& ns, const std::string& source_uri) {
    std::lock_guard<std::mutex> lock(mu_);
    blocks_.emplace_back(ns, source_uri);
}

Result<int> InMemoryBlockCleaner::CleanupSourceBlocks(const NsId& ns,
                                                      const std::string& source_prefix) {
    std::lock_guard<std::mutex> lock(mu_);
    int removed = 0;
    std::vector<std::pair<NsId, std::string>> kept;
    kept.reserve(blocks_.size());
    for (auto& [bns, uri] : blocks_) {
        // Match this namespace AND the exact table prefix (LIKE '<prefix>%').
        const bool match = (bns == ns) && uri.rfind(source_prefix, 0) == 0;
        if (match) {
            ++removed;
        } else {
            kept.emplace_back(bns, uri);
        }
    }
    blocks_ = std::move(kept);
    return removed;
}

int InMemoryBlockCleaner::RemainingCount(const NsId& ns) const {
    std::lock_guard<std::mutex> lock(mu_);
    int n = 0;
    for (const auto& [bns, uri] : blocks_) {
        (void)uri;
        if (bns == ns) ++n;
    }
    return n;
}

}  // namespace cortrix::import
