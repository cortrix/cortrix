#pragma once
#include <gmock/gmock.h>
#include "cortrix/spc/spc_manager.h"

namespace cortrix {
namespace testing {

class MockSPCManager : public SPCManager {
public:
    MockSPCManager() : SPCManager() {}

    MOCK_METHOD(Status, Submit, (std::shared_ptr<SPCTask> task), (override));
    MOCK_METHOD(int, CancelBySourcePath, (const std::string& source_path), (override));
    MOCK_METHOD(void, Start, (), (override));
    MOCK_METHOD(void, Stop, (), (override));
    MOCK_METHOD(size_t, QueueSize, (), (const, override));
    MOCK_METHOD(SPCStage, GetTaskStage, (int64_t task_id), (const, override));
    MOCK_METHOD(int, ProcessParsedDoc, (spc::ParsedDoc& parsed, SPCTask& task), (override));
};

}  // namespace testing
}  // namespace cortrix
