#include <gtest/gtest.h>

#include <type_traits>

#include "cortrix/catalog/i_unit_scatter_executor.h"

// S2.1/S2.3 coverage (F12 design test case #5): IUnitScatterExecutor is reserved
// as an ABSTRACT interface only — Phase 1 ships no concrete executor (1 NS = 1
// Unit needs no scatter). This pins that contract: the type exists, is abstract,
// has a virtual destructor, and there is intentionally no Default*Executor in the
// catalog module to instantiate.
namespace cortrix::catalog {
namespace {

TEST(UnitScatterExecutorTest, IsAbstractInterfaceOnly) {
    EXPECT_TRUE(std::is_abstract_v<IUnitScatterExecutor>);
    EXPECT_TRUE(std::has_virtual_destructor_v<IUnitScatterExecutor>);
    // Not default-constructible (pure virtual) — a Phase-1 impl would be a defect.
    EXPECT_FALSE(std::is_default_constructible_v<IUnitScatterExecutor>);
}

}  // namespace
}  // namespace cortrix::catalog
