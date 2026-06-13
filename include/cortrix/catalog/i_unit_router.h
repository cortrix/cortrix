#pragma once
#include <string>

#include "cortrix/catalog/catalog_types.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

namespace cortrix::catalog {

/// Unit lifecycle + runtime stats (F12 §3.2). The catalog is the SoT for Unit
/// metadata; F03 (enrichment), F25 (write) etc. update stats / state through
/// this interface rather than touching the units table directly.
///
/// Error model (F-FREEZE-1): value methods return Result<T> (error path = Status
/// carrying a CX_ERR_* token); void-fallible methods return Status.
///
/// Phase 1: every Unit is `active`; UpdateUnitState only accepts active→active,
/// and ShouldSeal always returns false (seal thresholds default to INT_MAX). The
/// full state machine (sealing → sealed → archived) activates in Phase 2.
class IUnitRouter {
public:
    virtual ~IUnitRouter() = default;

    /// The Unit's full descriptor. Error: CX_ERR_UNIT_NOT_FOUND.
    virtual Result<UnitDescriptor> GetUnit(const std::string& unit_id) const = 0;

    /// The Unit's current state. Error: CX_ERR_UNIT_NOT_FOUND.
    virtual Result<UnitState> GetUnitState(const std::string& unit_id) const = 0;

    /// Transition a Unit's state. Phase 1: only active→active is accepted; other
    /// transitions return CX_ERR_NOT_IMPLEMENTED (Phase 2 implements the machine).
    /// Error: CX_ERR_UNIT_NOT_FOUND if the Unit is absent.
    virtual Status UpdateUnitState(const std::string& unit_id,
                                   UnitState new_state) = 0;

    /// Update runtime stats (vector_count / size_bytes / last_write_at).
    /// Error: CX_ERR_UNIT_NOT_FOUND.
    virtual Status UpdateUnitStats(const std::string& unit_id,
                                   const UnitStats& stats) = 0;

    /// Whether the Unit has crossed a seal threshold. Phase 1: always false
    /// (thresholds default to INT_MAX). Error: CX_ERR_UNIT_NOT_FOUND.
    virtual Result<bool> ShouldSeal(const std::string& unit_id) const = 0;
};

}  // namespace cortrix::catalog
