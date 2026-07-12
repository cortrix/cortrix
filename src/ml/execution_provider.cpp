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

}  // namespace cortrix::ml
