#pragma once
#include <cstdint>

namespace cortrix::store {

/// P-HNSW tuning knobs. Split into the HNSW graph parameters (shared
/// with the index-agnostic IndexConfig) and the persistence parameters that are
/// specific to P-HNSW's WAL / group-commit / snapshot machinery. The latter are
/// intentionally NOT in the shared IndexConfig (iindex_factory.h) — they are an
/// Implementation detail sourced from PhnswConfig / IGlobalConfig.
struct PhnswConfig {
    // --- HNSW graph parameters ---
    int dim = 1024;
    int M = 16;
    int ef_construction = 200;
    int ef_search = 100;
    int max_elements = 1000000;

    // --- Group commit parameters ---
    int group_commit_max_batch = 64;
    int group_commit_timeout_ms = 5;
    int wal_buf_size_kb = 64;       ///< WAL write buffer size

    // --- Snapshot parameters ---
    std::int64_t snapshot_max_wal_size_mb = 64;
    int snapshot_max_wal_entries = 10000;
    int snapshot_keep_count = 2;
};

}  // namespace cortrix::store
