#pragma once

#include <string>

#include "cortrix/common/status.h"

namespace cortrix::ml {

enum class ExecutionProvider {
    kAuto,
    kCpu,
    kCoreMl,
    kCuda,
};

struct ExecutionProviderRuntimeState {
    bool ready = false;
    bool fallback = false;
    bool policy_mismatch = false;
    std::string configured_ep;
    std::string active_ep;
    std::string preferred_ep;
};

Status ParseExecutionProvider(const std::string& value, ExecutionProvider* provider);
const char* ExecutionProviderName(ExecutionProvider provider);
bool ExecutionProviderCompiled(ExecutionProvider provider);
ExecutionProvider PreferredAutoExecutionProvider();
Status ValidateExecutionProviderForBuild(ExecutionProvider provider);
ExecutionProviderRuntimeState EvaluateExecutionProviderRuntimeState(
    const std::string& configured_ep,
    const std::string& active_ep,
    bool model_configured,
    ExecutionProvider preferred_auto = PreferredAutoExecutionProvider());

}  // namespace cortrix::ml
