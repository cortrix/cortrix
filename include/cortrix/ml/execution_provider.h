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

Status ParseExecutionProvider(const std::string& value, ExecutionProvider* provider);
const char* ExecutionProviderName(ExecutionProvider provider);
bool ExecutionProviderCompiled(ExecutionProvider provider);
ExecutionProvider PreferredAutoExecutionProvider();
Status ValidateExecutionProviderForBuild(ExecutionProvider provider);

}  // namespace cortrix::ml
