#include "cortrix/query/scatter_plan.h"

#include <algorithm>

namespace cortrix::query {

std::optional<int> PlanMaxNamespacesOf(const AuthContext& /*auth*/) {
    // 🚩 integration FLAG: the frozen CE AuthContext has no plan_max_namespaces field. The
    // The cloud auth path injects it later; until then there is no plan cap, so the
    // effective cap is purely the global GUC. Returning nullopt keeps the
    // min(take-the-smaller) dormant without misreporting a cap of 0/unlimited.
    return std::nullopt;
}

int EffectiveMaxNamespacesWithPlan(int global_cap, std::optional<int> plan_cap) {
    int cap = global_cap;
    if (plan_cap.has_value()) cap = std::min(cap, *plan_cap);
    return cap < 1 ? 1 : cap;
}

int EffectiveMaxNamespaces(int global_cap, const AuthContext& auth) {
    return EffectiveMaxNamespacesWithPlan(global_cap, PlanMaxNamespacesOf(auth));
}

}  // namespace cortrix::query
