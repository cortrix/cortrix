#include <gtest/gtest.h>

#include <string>

#include "cortrix/spc_enricher/budget_tracker.h"

// Extra deterministic edges/matrices for the F-enricher BudgetTracker + the
// static ModelPricing table (basic disabled/cap-reached/injected cases live in
// test_budget_tracker.cpp). New unique suites: ModelPricingExtraMatrix /
// BudgetTrackerExtra. Costs use the ACTUAL embedded V1 prices (per-1K micro-USD):
//   gpt-4o-mini   {150, 600}
//   gpt-4o        {2500, 10000}
//   gpt-3.5-turbo {500, 1500}
//   default/unknown {2500, 10000}
// CostMicroUsd = (in*in_price + out*out_price)/1000  (integer round-down,
// negative tokens clamped to 0).

namespace cortrix::spc {
namespace {

// ---- ModelPricing::CostMicroUsd exact-arithmetic matrix ----------------------

struct ModelPricingExtraCase {
    std::string name;
    std::string model;
    int in_tokens;
    int out_tokens;
    int64_t expect_micro;
};

class ModelPricingExtraMatrix
    : public ::testing::TestWithParam<ModelPricingExtraCase> {};

TEST_P(ModelPricingExtraMatrix, Cost) {
    const auto& c = GetParam();
    ModelPricing pricing;
    EXPECT_EQ(pricing.CostMicroUsd(c.model, c.in_tokens, c.out_tokens),
              c.expect_micro)
        << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Pricing, ModelPricingExtraMatrix,
    ::testing::Values(
        // gpt-4o-mini: (1000*150 + 1000*600)/1000 = 750
        ModelPricingExtraCase{"mini_1k_1k", "gpt-4o-mini", 1000, 1000, 750},
        // gpt-4o: (1000*2500 + 1000*10000)/1000 = 12500
        ModelPricingExtraCase{"4o_1k_1k", "gpt-4o", 1000, 1000, 12500},
        // gpt-3.5-turbo: (1000*500 + 1000*1500)/1000 = 2000
        ModelPricingExtraCase{"turbo_1k_1k", "gpt-3.5-turbo", 1000, 1000, 2000},
        // unknown -> default {2500,10000}, same as gpt-4o
        ModelPricingExtraCase{"unknown_1k_1k", "no-such-model", 1000, 1000, 12500},
        // zero tokens -> zero cost
        ModelPricingExtraCase{"zero_tokens", "gpt-4o", 0, 0, 0},
        // negative tokens clamped to 0
        ModelPricingExtraCase{"neg_in_clamped", "gpt-4o", -100, 0, 0},
        ModelPricingExtraCase{"neg_out_clamped", "gpt-4o", 0, -100, 0},
        ModelPricingExtraCase{"neg_both_clamped", "gpt-4o", -5, -5, 0},
        // integer round-down: (1*150 + 0)/1000 = 0
        ModelPricingExtraCase{"mini_one_in_token_rounds_down", "gpt-4o-mini", 1, 0, 0},
        // (10*150)/1000 = 1 (1500/1000)
        ModelPricingExtraCase{"mini_ten_in_tokens", "gpt-4o-mini", 10, 0, 1},
        // output-only: (1000*600)/1000 = 600
        ModelPricingExtraCase{"mini_out_only", "gpt-4o-mini", 0, 1000, 600}),
    [](const ::testing::TestParamInfo<ModelPricingExtraCase>& i) { return i.param.name; });

// ---- PriceOf known/unknown ---------------------------------------------------

TEST(ModelPricingExtraDirect, PriceOfKnownAndUnknownFallback) {
    ModelPricing pricing;
    EXPECT_EQ(pricing.PriceOf("gpt-4o-mini").input_micro_usd_per_1k, 150);
    EXPECT_EQ(pricing.PriceOf("gpt-4o-mini").output_micro_usd_per_1k, 600);
    // unknown falls back to the conservative default (>= priciest known)
    EXPECT_EQ(pricing.PriceOf("mystery").input_micro_usd_per_1k, 2500);
    EXPECT_EQ(pricing.PriceOf("mystery").output_micro_usd_per_1k, 10000);
}

TEST(ModelPricingExtraDirect, SetOverridesExistingAndAddsNew) {
    ModelPricing pricing;
    pricing.Set("gpt-4o", ModelPrice{1, 2});  // override
    pricing.Set("custom", ModelPrice{7, 9});  // add
    EXPECT_EQ(pricing.PriceOf("gpt-4o").input_micro_usd_per_1k, 1);
    EXPECT_EQ(pricing.PriceOf("custom").output_micro_usd_per_1k, 9);
}

// ---- BudgetTracker cap gate edges --------------------------------------------

TEST(BudgetTrackerExtra, ZeroCapAlwaysAllowsEvenAfterSpend) {
    BudgetTracker bt(0);  // disabled
    EXPECT_FALSE(bt.enabled());
    EXPECT_TRUE(bt.AllowRequest());
    bt.RecordUsage("gpt-4o", 1000000, 1000000);  // big spend
    EXPECT_TRUE(bt.AllowRequest());  // still allowed (cap disabled)
}

TEST(BudgetTrackerExtra, NegativeCapDisabled) {
    BudgetTracker bt(-10);
    EXPECT_FALSE(bt.enabled());
    EXPECT_EQ(bt.budget_cap_usd(), 0);
    EXPECT_TRUE(bt.AllowRequest());
}

TEST(BudgetTrackerExtra, EnabledCapReportsWholeUsd) {
    BudgetTracker bt(5);
    EXPECT_TRUE(bt.enabled());
    EXPECT_EQ(bt.budget_cap_usd(), 5);
    EXPECT_EQ(bt.TotalCostMicroUsd(), 0);
}

TEST(BudgetTrackerExtra, AllowsWhileStrictlyUnderCapThenBlocksAtCap) {
    // cap = 1 USD = 1,000,000 micro. gpt-4o 1k/1k = 12,500 micro per record.
    BudgetTracker bt(1);
    int records = 0;
    // 1_000_000 / 12_500 = 80 records reach exactly the cap.
    for (int i = 0; i < 79; ++i) {
        ASSERT_TRUE(bt.AllowRequest()) << "record " << i;
        bt.RecordUsage("gpt-4o", 1000, 1000);
        ++records;
    }
    // after 79 records cost = 987,500 < 1,000,000 -> still allowed
    EXPECT_TRUE(bt.AllowRequest());
    bt.RecordUsage("gpt-4o", 1000, 1000);  // 80th -> 1,000,000 == cap
    ++records;
    EXPECT_EQ(records, 80);
    EXPECT_EQ(bt.TotalCostMicroUsd(), 1000000);
    EXPECT_FALSE(bt.AllowRequest());  // cost reached cap (strict < fails)
}

TEST(BudgetTrackerExtra, AccumulatesAcrossModels) {
    BudgetTracker bt(100);  // generous cap
    bt.RecordUsage("gpt-4o-mini", 1000, 1000);   // 750
    bt.RecordUsage("gpt-3.5-turbo", 1000, 1000); // 2000
    bt.RecordUsage("gpt-4o", 1000, 1000);        // 12500
    EXPECT_EQ(bt.TotalCostMicroUsd(), 750 + 2000 + 12500);
    EXPECT_TRUE(bt.AllowRequest());
}

TEST(BudgetTrackerExtra, InjectedPricingUsedForAccounting) {
    ModelPricing pricing;
    pricing.Set("cheap", ModelPrice{10, 10});
    BudgetTracker bt(1, pricing);
    bt.RecordUsage("cheap", 1000, 1000);  // (1000*10+1000*10)/1000 = 20
    EXPECT_EQ(bt.TotalCostMicroUsd(), 20);
    EXPECT_TRUE(bt.AllowRequest());
}

}  // namespace
}  // namespace cortrix::spc
