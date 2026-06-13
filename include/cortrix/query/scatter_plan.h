#pragma once
#include <optional>

#include "cortrix/auth/auth_context.h"  // cortrix::AuthContext

namespace cortrix::query {

/// S4.3 — Cloud V1 P01-3 plan coordination (F04 §4.1 Step 2 / §2.7 / topic 1.5 + 4.5).
///
/// The per-query NS hard cap is `min(global GUC scatter.max_namespaces_per_query,
/// AuthContext.plan_max_namespaces)` so a Cloud plan can cap a tenant *below* the
/// global limit. This function computes that effective cap; ScatterGather then
/// passes it to AuthorizeNamespaces (which throws CX_ERR_TOO_MANY_NAMESPACES when
/// the request — or the ["*"] expansion — exceeds it).
///
/// 🚩 D3.5 FLAG: the frozen `cortrix::AuthContext` (auth/auth_context.h) does NOT yet
/// carry a `plan_max_namespaces` field (it is the CE single-tenant struct). P01-3
/// will inject it on the Cloud auth path. Standalone, the plan cap is supplied
/// out-of-band (PlanMaxNamespacesOf returns nullopt = "no plan cap" → effective cap
/// == the global GUC), so the min (take-smaller) logic is fully unit-testable now and drops in
/// unchanged once the field exists.
///
/// @return the principal's plan NS cap, or nullopt when none applies (CE / no plan).
std::optional<int> PlanMaxNamespacesOf(const AuthContext& auth);

/// Core min (take-smaller): effective cap = min(global_cap, *plan_cap) when a plan cap is
/// present; else global_cap. Floored at 1 (a cap < 1 is meaningless). Pure function
/// so the S4.3 logic is unit-testable independent of whether AuthContext carries the
/// field yet (topic 1.5 + S4.3).
int EffectiveMaxNamespacesWithPlan(int global_cap, std::optional<int> plan_cap);

/// effective cap = min(global_cap, PlanMaxNamespacesOf(auth)). Today (no plan field)
/// this == global_cap; the min (take-smaller) activates once P01-3 injects the field (D3.5).
int EffectiveMaxNamespaces(int global_cap, const AuthContext& auth);

}  // namespace cortrix::query
