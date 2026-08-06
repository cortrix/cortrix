#pragma once
#include <gmock/gmock.h>

#include <functional>
#include <string>

#include "cortrix/common/i_global_config.h"

namespace cortrix {

/// Shared gmock double for IGlobalConfig (scaffolding) so downstream
/// features can unit-test config-driven logic.
class MockGlobalConfig : public IGlobalConfig {
public:
    MOCK_METHOD(Result<std::string>, GetString, (const std::string& key), (const, override));
    MOCK_METHOD(Result<bool>, GetBool, (const std::string& key), (const, override));
    MOCK_METHOD(Result<int>, GetInt, (const std::string& key), (const, override));
    MOCK_METHOD(Result<float>, GetFloat, (const std::string& key), (const, override));
    MOCK_METHOD(void, OnChange, (std::function<void(const std::string&)> cb), (override));
};

}  // namespace cortrix
