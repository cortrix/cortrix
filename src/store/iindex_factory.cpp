#include "cortrix/store/iindex_factory.h"

#include <memory>
#include <utility>

#include "cortrix/store/phnsw.h"
#include "cortrix/store/phnsw/snapshot_manager.h"
#include "cortrix/store/phnsw_config.h"

namespace cortrix::store {

namespace {

// Translate the index-agnostic IndexConfig (HNSW graph knobs) into a PhnswConfig.
// The persistence-layer knobs (group commit, snapshot thresholds, WAL buffer)
// are an index implementation detail and keep their PhnswConfig defaults here —
// they are sourced from IGlobalConfig once the index wires that up, not from the
// shared IndexConfig contract.
PhnswConfig ToPhnswConfig(const IndexConfig& cfg) {
    PhnswConfig pc;
    pc.dim = cfg.dim;
    pc.M = cfg.M;
    pc.ef_construction = cfg.ef_construction;
    pc.ef_search = cfg.ef_search;
    pc.max_elements = cfg.max_elements;
    return pc;
}

}  // namespace

Result<std::unique_ptr<IIndex>> PhnswIndexFactory::Create(const std::string& unit_data_dir,
                                                          const IndexConfig& config) {
    auto index = std::make_unique<PHnsw>(unit_data_dir, ToPhnswConfig(config));
    return Result<std::unique_ptr<IIndex>>(std::move(index));
}

Result<std::unique_ptr<IIndex>> PhnswIndexFactory::Open(const std::string& unit_data_dir) {
    // Open loads an existing Unit (its ctor runs Recover).-NEW-01: read the
    // persisted dim/M from the newest snapshot's .meta footer so the index is
    // reconstructed with the Unit's real graph parameters (an L2Space of the right
    // dimension) instead of assuming defaults. On a fresh Unit with no snapshot
    // yet, fall back to defaults — Open then mirrors Create over an empty dir.
    PhnswConfig pc;  // defaults (dim=1024, M=16, ...)
    SnapshotManager snap_mgr(unit_data_dir, /*keep_count=*/1);
    SnapshotManager::SnapshotMeta meta;
    if (snap_mgr.PeekLatestMeta(&meta) && meta.dim > 0) {
        pc.dim = meta.dim;
        pc.M = meta.M;
    }
    auto index = std::make_unique<PHnsw>(unit_data_dir, pc);
    Status recovered = index->Recover();
    if (!recovered.ok()) {
        return Result<std::unique_ptr<IIndex>>(recovered);
    }
    return Result<std::unique_ptr<IIndex>>(std::move(index));
}

}  // namespace cortrix::store
