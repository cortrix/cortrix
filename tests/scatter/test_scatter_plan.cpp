#include <gtest/gtest.h>

#include "cortrix/query/scatter_plan.h"

// Coverage: cloud plan coordination — effective cap = min(global GUC, plan cap).
namespace cortrix::query {
namespace {

// No plan cap → effective cap == the global GUC cap.
TEST(ScatterPlanTest, NoPlanCapKeepsGlobal) {
    EXPECT_EQ(EffectiveMaxNamespacesWithPlan(100, std::nullopt), 100);
}

// Plan cap lower than global → plan cap wins (the point of the feature).
TEST(ScatterPlanTest, PlanCapLowerWins) {
    EXPECT_EQ(EffectiveMaxNamespacesWithPlan(100, 10), 10);
}

// Plan cap higher than global → global still caps (can't exceed the global limit).
TEST(ScatterPlanTest, GlobalCapsAbovePlan) {
    EXPECT_EQ(EffectiveMaxNamespacesWithPlan(50, 500), 50);
}

// Result is floored at 1 (a cap < 1 is meaningless).
TEST(ScatterPlanTest, FlooredAtOne) {
    EXPECT_EQ(EffectiveMaxNamespacesWithPlan(0, std::nullopt), 1);
    EXPECT_EQ(EffectiveMaxNamespacesWithPlan(100, 0), 1);
}

// 🚩 integration: the frozen CE AuthContext has no plan field, so PlanMaxNamespacesOf is
// nullopt today → the AuthContext overload == the global cap. (Once the cloud path injects
// the field, the WithPlan logic above activates unchanged.)
TEST(ScatterPlanTest, AuthContextHasNoPlanCapYet) {
    AuthContext auth;
    auth.user_id = "u1";
    EXPECT_FALSE(PlanMaxNamespacesOf(auth).has_value());
    EXPECT_EQ(EffectiveMaxNamespaces(100, auth), 100);
}

}  // namespace
}  // namespace cortrix::query
