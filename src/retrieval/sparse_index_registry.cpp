#include "cortrix/retrieval/sparse_index_registry.h"

#include <filesystem>
#include <utility>

#include "cortrix/logging/logging.h"

namespace cortrix::retrieval {

SparseIndexRegistry::SparseIndexRegistry(std::string data_dir, SpladeConfig config)
    : data_dir_(std::move(data_dir)), config_(config) {}

std::string SparseIndexRegistry::PathFor(const std::string& ns_id) const {
    if (data_dir_.empty()) return ":memory:";
    std::filesystem::path p =
        std::filesystem::path(data_dir_) / ns_id / "sparse_index.db";
    return p.string();
}

ISparseRetriever* SparseIndexRegistry::GetOrOpen(const std::string& ns_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = by_ns_.find(ns_id);
    if (it != by_ns_.end()) return it->second.get();

    const std::string path = PathFor(ns_id);
    if (!data_dir_.empty()) {
        // Ensure the per-NS directory exists before SQLite opens the file.
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path(), ec);
        if (ec) {
            CORTRIX_LOG_WARN("main",
                             "sparse index dir create failed for ns '{}': {}",
                             ns_id, ec.message());
            return nullptr;
        }
    }

    auto retriever = std::make_unique<SpladeSparseRetriever>(config_, path);
    Status s = retriever->Open();
    if (!s.ok()) {
        CORTRIX_LOG_WARN("main", "sparse index open failed for ns '{}': {}",
                         ns_id, s.message());
        return nullptr;  // caller falls back to the non-sparse paths
    }
    ISparseRetriever* raw = retriever.get();
    by_ns_.emplace(ns_id, std::move(retriever));
    return raw;
}

}  // namespace cortrix::retrieval
