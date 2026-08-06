#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

namespace cortrix::spc {

/// Per-1K-token prices for a model, in micro-USD (1 USD = 1e6 micro-USD). V1
/// ships a small static table (design "V1 static model_pricing.json"); the
/// real file-loaded pricing is a Phase 2 / integration concern. Unknown models fall back
/// to a conservative default so budget accounting never silently undercounts.
struct ModelPrice {
    int64_t input_micro_usd_per_1k = 0;   ///< prompt tokens
    int64_t output_micro_usd_per_1k = 0;  ///< completion tokens
};

/// Static pricing registry (topic 3.5). Lookup is total: unknown model → default.
class ModelPricing {
public:
    ModelPricing();  // loads the built-in V1 table

    /// Price for `model` (unknown → the default fallback price).
    ModelPrice PriceOf(const std::string& model) const;

    /// Cost of (input_tokens, output_tokens) for `model`, in micro-USD.
    int64_t CostMicroUsd(const std::string& model, int input_tokens,
                         int output_tokens) const;

    /// Override / add a model price (tests / future config load).
    void Set(const std::string& model, ModelPrice price);

private:
    std::map<std::string, ModelPrice> prices_;
    ModelPrice default_price_;
};

/// BudgetTracker (design). Enforces a global USD spend cap across enrichment
/// calls. budget_cap_usd == 0 disables the cap (AllowRequest always true). Costs
/// accumulate in micro-USD (atomic) to avoid floating-point drift.
///
/// AllowRequest() is the pre-call gate: false once the accumulated cost has
/// reached the cap → caller returns CX_ERR_ENRICHER_BUDGET + degrades to Null.
class BudgetTracker {
public:
    /// @param budget_cap_usd  whole-USD cap (0 = unlimited). Converted to micro-USD.
    explicit BudgetTracker(int budget_cap_usd);

    /// Inject a custom pricing table (default = the built-in V1 ModelPricing).
    BudgetTracker(int budget_cap_usd, ModelPricing pricing);

    /// pre-call gate. True while under the cap (or cap disabled).
    bool AllowRequest() const;

    /// Add the cost of one call's usage (topic 3.5). Thread-safe.
    void RecordUsage(const std::string& model, int input_tokens, int output_tokens);

    /// Accumulated spend so far, in micro-USD / whole USD (whole = floor).
    int64_t TotalCostMicroUsd() const { return total_cost_micro_usd_.load(); }
    int budget_cap_usd() const { return static_cast<int>(budget_cap_micro_usd_ / 1'000'000); }
    bool enabled() const { return budget_cap_micro_usd_ > 0; }

private:
    std::atomic<int64_t> total_cost_micro_usd_{0};
    int64_t budget_cap_micro_usd_;  // 0 == disabled
    ModelPricing pricing_;
};

}  // namespace cortrix::spc
