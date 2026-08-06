#pragma once
#include <gmock/gmock.h>

#include <string>

#include "cortrix/llm/i_llm_client.h"

namespace cortrix::llm {

/// Shared gmock double for ILlmClient. Provided by scaffolding so the 7
/// downstream consumers can unit-test
/// their LLM-dependent logic without a live endpoint.
class MockLlmClient : public ILlmClient {
public:
    MOCK_METHOD(ChatCompletionResponse, Chat,
                (const std::string& prompt, const LlmCallConfig& config), (override));
};

}  // namespace cortrix::llm
