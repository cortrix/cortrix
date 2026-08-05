#pragma once
#include <cstdint>
#include <string>

#include "cortrix/store/write_coordinator.h"  // WriteCoordinatorConfig (per-NS coord tuning)

namespace cortrix::resource {

/// Per-Unit store.db PRAGMA tuning. Applied by the pool right
/// after each store.db Open (ApplyPragmas). The numeric SQLite knobs
/// follow SQLite's own semantics: `cache_size` negative = KiB (not pages).
struct SqlitePragmas {
    std::string journal_mode = "WAL";       ///< concurrent read/write, same model as the PWL
    std::string synchronous = "NORMAL";     ///< perf vs durability trade-off (FULL is 2-3x slower)
    int cache_size_kb = -2000;              ///< 2MB per Unit (SQLite negative = KiB)
    int64_t mmap_size_bytes = 64 * 1024 * 1024;  ///< 64MB mmap for large-file reads
    std::string temp_store = "MEMORY";      ///< temp tables in memory (no spill to disk)
    int64_t busy_timeout_ms = 5000;         ///< lock wait, avoids transient test failures
};

/// Global resource-pool configuration.
///
/// Scope is **global only**: a namespace cannot decide its own quota
/// rules, so there is no NS-level f05_config blob. In production this is sourced
/// from IGlobalConfig::GetF05Config() — that getter is a D3.5 reverse hook into
/// the shared scaffolding; standalone code (and tests) construct an
/// F05Config directly and pass it to the pool, so the pool stays decoupled from the
/// shared IGlobalConfig contract during D3.
struct F05Config {
    // topic 2 — NS count ceiling.
    size_t max_namespaces_per_instance = 64;

    // topic 3 — memory budget admission (0 = disabled, the default).
    size_t memory_budget_bytes = 0;

    // topic 5 — eager startup load (8 workers, per-NS load timeout).
    // The timeout is a guard against ONE hung NS stalling startup (§6.3 C1), not a
    // performance budget: a large-but-healthy NS legitimately needs many seconds to
    // load (open 100MB+ store.db, replay the HNSW WAL, mmap a multi-hundred-MB sparse
    // index, run per-Unit schema migrations). 5s was too aggressive — it mis-classified
    // big healthy namespaces as "hung" and abandoned them, leaving them invisible after
    // restart (their data is on disk but never admitted). 60s tolerates large NSs
    // (measured: a 5183-doc / 100MB-store / 329MB-sparse NS loads in ~42s) while still
    // bounding a truly hung load. Overridable via IGlobalConfig (config.yaml
    // `namespace.load_timeout_ms_per_ns`); <=0 means "no timeout, wait fully".
    int startup_load_workers = 8;
    int64_t load_timeout_ms_per_ns = 60000;

    // topic 7 — per-Unit store.db PRAGMAs.
    SqlitePragmas pragmas;

    // Root dir under which the pool derives a Unit's on-disk layout
    // (<data_root>/<unit_id>/{index, pending.wal, store.db}) when loading. The
    // *production* per-Unit path scheme is owned by the catalog / deployment and resolved
    // at D3.5; this knob keeps the standalone load path + tests deterministic.
    std::string data_root;

    // Per-NS WriteCoordinator tuning, passed to the WriteCoordinatorFactory when a
    // bundle's write coordinator is constructed.
    cortrix::store::WriteCoordinatorConfig write_coord_config;
};

}  // namespace cortrix::resource
