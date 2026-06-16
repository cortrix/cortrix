#include <cstdint>
#include "cortrix/spc_enricher/budget_tracker.h"

#include <utility>

namespace cortrix::spc {

ModelPricing::ModelPricing() {
    // Built-in V1 static pricing (topic 3.5). Public OpenAI list prices as of the
    // design baseline, expressed in micro-USD per 1K tokens. This is the V1
    // "model_pricing.json" embedded; a file-loaded / updatable table is Phase 2.
    prices_ = {
        {"gpt-4o-mini",   {150, 600}},      // $0.150 / $0.600 per 1M  → per-1K micro
        {"gpt-4o",        {2500, 10000}},   // $2.50  / $10.00 per 1M
        {"gpt-3.5-turbo", {500, 1500}},     // $0.50  / $1.50  per 1M
    };
    // Conservative default for unknown models (>= the priciest above) so spend is
    // never silently undercounted against the cap.
    default_price_ = {2500, 10000};
}

ModelPrice ModelPricing::PriceOf(const std::string& model) const {
    auto it = prices_.find(model);
    return it == prices_.end() ? default_price_ : it->second;
}

int64_t ModelPricing::CostMicroUsd(const std::string& model, int input_tokens,
                                   int output_tokens) const {
    const ModelPrice p = PriceOf(model);
    const int64_t in = input_tokens < 0 ? 0 : input_tokens;
    const int64_t out = output_tokens < 0 ? 0 : output_tokens;
    // micro_usd = tokens/1000 * price_per_1k ; integer math (round down).
    return (in * p.input_micro_usd_per_1k + out * p.output_micro_usd_per_1k) / 1000;
}

void ModelPricing::Set(const std::string& model, ModelPrice price) {
    prices_[model] = price;
}

BudgetTracker::BudgetTracker(int budget_cap_usd)
    : budget_cap_micro_usd_(static_cast<int64_t>(budget_cap_usd < 0 ? 0 : budget_cap_usd) *
                            1'000'000) {}

BudgetTracker::BudgetTracker(int budget_cap_usd, ModelPricing pricing)
    : budget_cap_micro_usd_(static_cast<int64_t>(budget_cap_usd < 0 ? 0 : budget_cap_usd) *
                            1'000'000),
      pricing_(std::move(pricing)) {}

bool BudgetTracker::AllowRequest() const {
    if (budget_cap_micro_usd_ <= 0) return true;  // cap disabled
    return total_cost_micro_usd_.load(std::memory_order_relaxed) < budget_cap_micro_usd_;
}

void BudgetTracker::RecordUsage(const std::string& model, int input_tokens,
                                int output_tokens) {
    const int64_t cost = pricing_.CostMicroUsd(model, input_tokens, output_tokens);
    total_cost_micro_usd_.fetch_add(cost, std::memory_order_relaxed);
}

}  // namespace cortrix::spc
