#pragma once
#include <gmock/gmock.h>

#include <string>

#include "cortrix/llm/i_llm_client.h"

namespace cortrix::llm {

/// Shared gmock double for ILlmClient. Provided by scaffolding D2-pre-3 so the 7
/// downstream consumers (F03 / F35 / F38 / F41 / F15 / MEM02 / F36) can unit-test
/// their LLM-dependent logic without a live endpoint.
class MockLlmClient : public ILlmClient {
public:
    MOCK_METHOD(ChatCompletionResponse, Chat,
                (const std::string& prompt, const LlmCallConfig& config), (override));
};

}  // namespace cortrix::llm
