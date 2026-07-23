#pragma once
#include <functional>
#include <string>

#include "cortrix/health/readiness.h"  // F20 readiness contract (ReadinessRegistry)

namespace httplib { class Server; }

namespace cortrix::deploy {

/// Register the dual health endpoints (F24 §4.2, F20-7-rev-2 — F24 owns the
/// endpoints; F20 owns the readiness *contract*).
///
/// Unified onto the F20 ReadinessRegistry: /ready now
/// calls `health::ReadinessRegistry::BuildReport()` (replacing F24's standalone
/// HealthProviders struct, which was a D3 standalone placeholder). The 5 readiness
/// components (catalog / vector_index / secret_provider / spc_pipeline /
/// memory_store, design sec 8.4) are registered with `registry` at main.cpp wiring
/// time as each owning Feature exposes its IsReady() probe (F20-S5); this endpoint
/// only aggregates them and maps overall readiness to 200 / 503.
///
///   GET /api/v1/system/health/live   — liveness: 200 once the process is up;
///       only fails if the process is wedged. Used by K8s livenessProbe (restart).
///   GET /api/v1/system/health/ready  — readiness: 200 only when NOT shutting
///       down AND every registered component is ready; else 503 with the failing
///       components. This is the docker-compose healthcheck path (start_period →
///       ready=false drops traffic but does not restart). Anonymous (no auth).
///
/// `registry` must outlive `server` (the handlers hold it by reference).
/// `is_shutting_down` may be null (→ treated as "not shutting down"). `version`,
/// when non-empty, is echoed into the /ready body alongside BuildReport()'s
/// {status, components, warnings}.
void RegisterHealthRoutes(httplib::Server& server,
                          const health::ReadinessRegistry& registry,
                          std::function<bool()> is_shutting_down = nullptr,
                          std::string version = "");

}  // namespace cortrix::deploy
