#include "cortrix/ml/execution_provider.h"

#include <algorithm>
#include <cctype>

namespace cortrix::ml {

Status ParseExecutionProvider(const std::string& value, ExecutionProvider* provider) {
    if (provider == nullptr) {
        return Status::InvalidArgument("execution provider output must not be null");
    }

    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized == "auto") {
        *provider = ExecutionProvider::kAuto;
    } else if (normalized == "cpu") {
        *provider = ExecutionProvider::kCpu;
    } else if (normalized == "coreml") {
        *provider = ExecutionProvider::kCoreMl;
    } else if (normalized == "cuda") {
        *provider = ExecutionProvider::kCuda;
    } else {
        return Status::InvalidArgument(
            "execution provider must be one of auto, cpu, coreml, cuda; got '" + value + "'");
    }
    return Status::Ok();
}

const char* ExecutionProviderName(ExecutionProvider provider) {
    switch (provider) {
        case ExecutionProvider::kAuto: return "auto";
        case ExecutionProvider::kCpu: return "cpu";
        case ExecutionProvider::kCoreMl: return "coreml";
        case ExecutionProvider::kCuda: return "cuda";
    }
    return "unknown";
}

bool ExecutionProviderCompiled(ExecutionProvider provider) {
    switch (provider) {
        case ExecutionProvider::kAuto:
        case ExecutionProvider::kCpu:
            return true;
        case ExecutionProvider::kCoreMl:
#ifdef CORTRIX_HAS_COREML
            return true;
#else
            return false;
#endif
        case ExecutionProvider::kCuda:
#ifdef CORTRIX_HAS_CUDA
            return true;
#else
            return false;
#endif
    }
    return false;
}

ExecutionProvider PreferredAutoExecutionProvider() {
#ifdef CORTRIX_HAS_CUDA
    return ExecutionProvider::kCuda;
#elif defined(CORTRIX_HAS_COREML)
    return ExecutionProvider::kCoreMl;
#else
    return ExecutionProvider::kCpu;
#endif
}

Status ValidateExecutionProviderForBuild(ExecutionProvider provider) {
    if (provider == ExecutionProvider::kAuto || ExecutionProviderCompiled(provider)) {
        return Status::Ok();
    }
    return Status::Unavailable(std::string("execution provider '") +
                               ExecutionProviderName(provider) +
                               "' is not compiled into this Cortrix build");
}

ExecutionProviderRuntimeState EvaluateExecutionProviderRuntimeState(
    const std::string& configured_ep,
    const std::string& active_ep,
    bool model_configured,
    ExecutionProvider preferred_auto) {
    ExecutionProviderRuntimeState state;
    state.configured_ep = configured_ep;
    state.active_ep = active_ep;
    state.preferred_ep = ExecutionProviderName(preferred_auto);

    ExecutionProvider configured_provider = ExecutionProvider::kAuto;
    if (!ParseExecutionProvider(configured_ep, &configured_provider).ok()) {
        state.policy_mismatch = true;
        return state;
    }
    state.configured_ep = ExecutionProviderName(configured_provider);

    if (!model_configured) {
        state.ready = active_ep == "stub";
        state.policy_mismatch = !state.ready;
        return state;
    }

    ExecutionProvider active_provider = ExecutionProvider::kAuto;
    if (!ParseExecutionProvider(active_ep, &active_provider).ok() ||
        active_provider == ExecutionProvider::kAuto) {
        state.policy_mismatch = true;
        return state;
    }
    state.active_ep = ExecutionProviderName(active_provider);

    if (configured_provider != ExecutionProvider::kAuto) {
        state.ready = active_provider == configured_provider;
        state.policy_mismatch = !state.ready;
        return state;
    }

    if (preferred_auto == ExecutionProvider::kAuto) {
        state.policy_mismatch = true;
        return state;
    }

    state.fallback = preferred_auto != ExecutionProvider::kCpu &&
                     active_provider == ExecutionProvider::kCpu;
    state.ready = active_provider == preferred_auto || state.fallback;
    state.policy_mismatch = !state.ready;
    return state;
}

}  // namespace cortrix::ml
