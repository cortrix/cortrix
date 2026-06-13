#include "cortrix/health/readiness.h"

#include <utility>

namespace cortrix::health {

namespace {

/// Adapts a name + probe lambda into an IReadyComponent so callers can register
/// readiness without a dedicated class.
class LambdaComponent : public IReadyComponent {
public:
    LambdaComponent(std::string name, std::function<ComponentReadiness()> probe)
        : name_(std::move(name)), probe_(std::move(probe)) {}

    std::string name() const override { return name_; }
    ComponentReadiness CheckReady() const override { return probe_(); }

private:
    std::string name_;
    std::function<ComponentReadiness()> probe_;
};

}  // namespace

void ReadinessRegistry::Register(std::shared_ptr<IReadyComponent> component) {
    if (component) {
        components_.push_back(std::move(component));
    }
}

void ReadinessRegistry::Register(std::string name,
                                 std::function<ComponentReadiness()> probe) {
    components_.push_back(
        std::make_shared<LambdaComponent>(std::move(name), std::move(probe)));
}

nlohmann::json ReadinessRegistry::BuildReport(bool* overall_ready) const {
    nlohmann::json body;
    body["components"] = nlohmann::json::object();
    nlohmann::json warnings = nlohmann::json::array();

    bool all_ready = true;
    for (const auto& component : components_) {
        ComponentReadiness r = component->CheckReady();
        nlohmann::json entry = r.detail;
        entry["status"] = r.ready ? "ok" : "not_ready";
        body["components"][component->name()] = entry;
        if (!r.ready) {
            all_ready = false;
            warnings.push_back(component->name() + " not ready");
        }
    }

    body["status"] = all_ready ? "ready" : "not_ready";
    if (!warnings.empty()) {
        body["warnings"] = warnings;
    }
    if (overall_ready) {
        *overall_ready = all_ready;
    }
    return body;
}

}  // namespace cortrix::health
