#pragma once
#include <gmock/gmock.h>
#include "cortrix/store/cortrix_blob_store.h"

namespace cortrix {
namespace testing {

class MockBlobStore : public CortrixBlobStore {
public:
    MOCK_METHOD(int, store, (const std::string& ns, int64_t doc_id, const void* data, size_t len), (override));
    MOCK_METHOD(int, load, (const std::string& ns, int64_t doc_id, std::string& data), (override));
    MOCK_METHOD(int, del, (const std::string& ns, int64_t doc_id), (override));
};

}  // namespace testing
}  // namespace cortrix
