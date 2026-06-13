#pragma once
#include <gmock/gmock.h>
#include "cortrix/memory/memory_store.h"

namespace cortrix {
namespace testing {

// MemoryStore is not a virtual interface, so we provide usage guidance.
// For unit tests, mock the underlying CortrixStore (MockCortrixStore) and
// create MemoryStore with the mock:
//
//   MockCortrixStore mock_store;
//   MemoryStore memory_store(mock_store);
//   memory_store.Init();
//
// This allows testing MemoryStore's SQL generation and logic
// without a real SQLite database.

}  // namespace testing
}  // namespace cortrix
