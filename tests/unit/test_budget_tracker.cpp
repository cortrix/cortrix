#include "cortrix/spc_enricher/budget_tracker.h"

#include <gtest/gtest.h>

namespace cortrix::spc {
namespace {

TEST(ModelPricingTest, KnownAndUnknownModels) {
    ModelPricing p;
    // gpt-4o-mini: 150 / 600 micro-USD per 1K. 1000 in + 1000 out = 150 + 600.
    EXPECT_EQ(p.CostMicroUsd("gpt-4o-mini", 1000, 1000), 750);
    // Unknown model → conservative default (2500/10000).
    EXPECT_EQ(p.CostMicroUsd("mystery-model", 1000, 1000), 12500);
    // Zero / negative tokens → 0 cost (no underflow).
    EXPECT_EQ(p.CostMicroUsd("gpt-4o-mini", 0, 0), 0);
    EXPECT_EQ(p.CostMicroUsd("gpt-4o-mini", -5, -5), 0);
}

TEST(ModelPricingTest, SetOverride) {
    ModelPricing p;
    p.Set("custom", {1000, 2000});
    EXPECT_EQ(p.CostMicroUsd("custom", 1000, 1000), 3000);
}

TEST(BudgetTrackerTest, DisabledWhenCapZero) {
    BudgetTracker bt(0);
    EXPECT_FALSE(bt.enabled());
    EXPECT_TRUE(bt.AllowRequest());
    // Even after recording usage, an uncapped tracker always allows.
    bt.RecordUsage("gpt-4o", 1'000'000, 1'000'000);
    EXPECT_TRUE(bt.AllowRequest());
}

TEST(BudgetTrackerTest, NegativeCapTreatedAsDisabled) {
    BudgetTracker bt(-5);
    EXPECT_FALSE(bt.enabled());
    EXPECT_TRUE(bt.AllowRequest());
}

TEST(BudgetTrackerTest, AllowsUntilCapReached) {
    // $1 cap. gpt-4o-mini: 1M in tokens = 1000 * 150 = 150000 micro = $0.15.
    BudgetTracker bt(1);
    EXPECT_TRUE(bt.enabled());
    EXPECT_EQ(bt.budget_cap_usd(), 1);
    EXPECT_TRUE(bt.AllowRequest());

    // Accrue ~$0.90 (6 × $0.15) — still under $1.
    for (int i = 0; i < 6; ++i) bt.RecordUsage("gpt-4o-mini", 1'000'000, 0);
    EXPECT_TRUE(bt.AllowRequest());
    EXPECT_EQ(bt.TotalCostMicroUsd(), 900'000);

    // One more (~$0.15) crosses $1 → cap reached, AllowRequest false.
    bt.RecordUsage("gpt-4o-mini", 1'000'000, 0);
    EXPECT_GE(bt.TotalCostMicroUsd(), 1'000'000);
    EXPECT_FALSE(bt.AllowRequest());
}

TEST(BudgetTrackerTest, InjectedPricing) {
    ModelPricing pricing;
    pricing.Set("cheap", {1, 1});  // 1 micro-USD per 1K each
    BudgetTracker bt(1, pricing);
    bt.RecordUsage("cheap", 1000, 1000);  // 2 micro-USD
    EXPECT_EQ(bt.TotalCostMicroUsd(), 2);
    EXPECT_TRUE(bt.AllowRequest());  // nowhere near $1
}

}  // namespace
}  // namespace cortrix::spc
