#pragma once
#include <mutex>
#include <string>

#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/i_unit_router.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::catalog {

/// catalog.db-backed IUnitRouter. Borrows the sqlite3
/// handle owned by CatalogDb. Phase 1: states are all `active`, ShouldSeal is
/// always false (thresholds default to INT_MAX), and UpdateUnitState accepts only
/// active→active (other transitions → CX_ERR_NOT_IMPLEMENTED until Phase 2).
class DefaultUnitRouter : public IUnitRouter {
public:
    explicit DefaultUnitRouter(sqlite3* db);

    Result<UnitDescriptor> GetUnit(const std::string& unit_id) const override;
    Result<UnitState> GetUnitState(const std::string& unit_id) const override;
    Status UpdateUnitState(const std::string& unit_id, UnitState new_state) override;
    Status UpdateUnitStats(const std::string& unit_id, const UnitStats& stats) override;
    Result<bool> ShouldSeal(const std::string& unit_id) const override;

private:
    Result<UnitDescriptor> ReadUnitLocked(const std::string& unit_id) const;

    sqlite3* db_;            // borrowed, not owned
    mutable std::mutex mu_;
};

}  // namespace cortrix::catalog
