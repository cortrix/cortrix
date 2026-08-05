#pragma once
#include <string>

namespace cortrix::query {
// Forward declarations ONLY. These query-layer types are not defined in
// the catalog's Layer-0 scope, and this interface has NO Phase-1 implementation
// ("declaration only"), so forward declarations are sufficient and keep the catalog building
// standalone. Phase 2's MultiUnitNSExecutor supplies the concrete types +
// the implementation.
struct QueryContext;
struct UnitQueryResult;
}  // namespace cortrix::query

namespace cortrix::catalog {

/// Per-Unit scatter execution (separate NS / Unit scatter
/// interfaces). DECLARATION ONLY in Phase 1: 1 NS = 1 Unit, so queries route
/// directly to the single Unit and no scatter is needed. Phase 2 implements this
/// inside MultiUnitNSExecutor (the per-Unit fan-out of a 1:N namespace).
///
/// Reserved here so the contract surface is stable across phases; there is no
/// Default*Executor in Phase 1 (a test asserts no concrete impl is shipped yet).
class IUnitScatterExecutor {
public:
    virtual ~IUnitScatterExecutor() = default;

    /// Execute the query against one Unit. `oversample` (Phase 1 default 1.0)
    /// becomes Unit-count-adaptive in Phase 2 (prereq constraint 1).
    virtual query::UnitQueryResult ExecuteForUnit(
        const query::QueryContext& ctx,
        const std::string& unit_id,
        float oversample = 1.0f) = 0;
};

}  // namespace cortrix::catalog
