#pragma once
#include <memory>
#include <string>

#include "cortrix/common/result.h"
#include "cortrix/store/iindex.h"

namespace cortrix::store {

enum class IndexType {
    kPhnsw = 1,          ///< Phase 1: the only implementation
    kBruteForce = 2,     ///< Phase 2: small / active units
    kQuantizedHnsw = 3,  ///< Phase 2: archived units
};

/// Index-agnostic open/create configuration shared by all IIndex backends.
/// Backend-specific tuning (group-commit batch size, snapshot thresholds, WAL
/// buffer, ...) lives in that backend's own config (e.g. PhnswConfig sourced
/// from IGlobalConfig), NOT in this shared contract.
struct IndexConfig {
    IndexType type = IndexType::kPhnsw;
    int dim = 1024;
    int M = 16;
    int ef_construction = 200;
    int ef_search = 100;
    int max_elements = 1000000;
};

/// Factory for IIndex instances. The index layer provides the
/// concrete PhnswIndexFactory; the catalog (OpenUnitIndex) and NamespacePool
/// (Acquire) hold an IIndexFactory* and call Open(unit_data_dir) per Unit.
class IIndexFactory {
public:
    virtual ~IIndexFactory() = default;

    /// Create a fresh index under unit_data_dir (first registration of a Unit).
    virtual Result<std::unique_ptr<IIndex>> Create(const std::string& unit_data_dir,
                                                   const IndexConfig& config) = 0;

    /// Open an existing index (loads snapshot + replays WAL recovery). On
    /// failure returns the recovery error (e.g. CX_ERR_PHNSW_RECOVERY_FAILED).
    virtual Result<std::unique_ptr<IIndex>> Open(const std::string& unit_data_dir) = 0;
};

/// Phase 1 P-HNSW factory. This skeleton (interface + declarations) is delivered
/// by scaffolding so the catalog and pool can compile and mock against it now; the
/// concrete P-HNSW implementation is delivered by the index layer, which replaces
/// the placeholder bodies in iindex_factory.cpp.
class PhnswIndexFactory : public IIndexFactory {
public:
    Result<std::unique_ptr<IIndex>> Create(const std::string& unit_data_dir,
                                           const IndexConfig& config) override;
    Result<std::unique_ptr<IIndex>> Open(const std::string& unit_data_dir) override;
};

}  // namespace cortrix::store
