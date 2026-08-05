#pragma once
// Security hardening — readiness contract.
//
// BOUNDARY (D-R1 brief sec 3.2): the /live + /ready HTTP endpoints are owned by
// the deployment layer (RegisterHealthRoutes). This owns only the *contract*: the IReadyComponent
// interface each subsystem implements and the ReadinessRegistry that aggregates
// them into the /ready response body. The endpoint calls
// ReadinessRegistry::BuildReport() and maps overall readiness to 200 / 503.
//
// The 5 readiness components are: catalog (IBloomFilter +
// catalog.db), vector_index (model load + WAL replay), secret_provider
// (ISecretProvider::IsHealthy), spc_pipeline (parser / enricher workers),
// memory_store. This layer ships the secret_provider adapter; the others are
// registered by their owning Features in D3.5 (deferred).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cortrix::health {

/// Outcome of one component's readiness probe. `ready` drives the overall HTTP
/// status; `detail` is merged verbatim into the component's object in the
/// /ready body (e.g. {"bloom_filter_ready": true} or {"reason": "model_loading",
/// "progress": 0.65}).
struct ComponentReadiness {
    bool ready = false;
    nlohmann::json detail = nlohmann::json::object();
};

/// A subsystem that can report readiness to the /ready endpoint. Implementations
/// must be thread-safe (probed from request threads).
class IReadyComponent {
public:
    virtual ~IReadyComponent() = default;

    /// Stable component key used in the /ready body (e.g. "secret_provider").
    virtual std::string name() const = 0;

    /// Probe current readiness. Must be cheap and non-blocking.
    virtual ComponentReadiness CheckReady() const = 0;
};

/// Aggregates registered components into the /ready report. Owned by the server
/// and populated at startup; the health endpoint reads it per request.
class ReadinessRegistry {
public:
    /// Register a component. Order is preserved in the report. Components keep
    /// their own lifetime; the registry holds shared ownership.
    void Register(std::shared_ptr<IReadyComponent> component);

    /// Convenience: register a component from a name + probe lambda, for callers
    /// that don't want a dedicated class.
    void Register(std::string name, std::function<ComponentReadiness()> probe);

    /// Build the /ready body. `overall_ready` is set to true iff every component
    /// is ready. Shape matches design sec 8.3:
    ///   {"status": "ready"|"not_ready", "components": {<name>: {"status": ...}},
    ///    "warnings": [...]}
    /// The caller adds uptime_seconds / version and maps status -> 200 / 503.
    nlohmann::json BuildReport(bool* overall_ready) const;

    size_t size() const { return components_.size(); }

private:
    std::vector<std::shared_ptr<IReadyComponent>> components_;
};

}  // namespace cortrix::health
