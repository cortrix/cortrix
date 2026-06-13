#pragma once
#include <gmock/gmock.h>
#include "cortrix/store/cortrix_store.h"

namespace cortrix {
namespace testing {

class MockCortrixStore : public CortrixStore {
public:
    // Lifecycle
    MOCK_METHOD(int, Open, (const std::string& db_path), (override));
    MOCK_METHOD(void, Close, (), (override));

    // Document CRUD
    MOCK_METHOD(int, doc_create, (CortrixDoc& doc), (override));
    MOCK_METHOD(int, doc_get, (int64_t doc_id, CortrixDoc& doc), (override));
    MOCK_METHOD(int, doc_list, (std::vector<CortrixDoc>& docs), (override));
    MOCK_METHOD(int, doc_update_status, (int64_t doc_id, DocStatus status, const std::string& error_msg), (override));
    MOCK_METHOD(int, doc_delete, (int64_t doc_id), (override));
    MOCK_METHOD(int, doc_find_by_source, (const std::string& source_type, const std::string& source_path, CortrixDoc& doc), (override));
    MOCK_METHOD(int, doc_find_by_hash, (const std::string& content_hash, CortrixDoc& doc), (override));
    MOCK_METHOD(int, doc_count, (int64_t* count), (override));

    // Block CRUD
    MOCK_METHOD(int, block_insert, (CortrixBlock& block), (override));
    MOCK_METHOD(int, block_get, (uint64_t block_id, CortrixBlock& block), (override));
    MOCK_METHOD(int, block_list_by_doc, (int64_t doc_id, std::vector<CortrixBlock>& blocks), (override));
    MOCK_METHOD(int, block_delete_by_doc, (int64_t doc_id), (override));
    MOCK_METHOD(int, block_count_by_doc, (int64_t doc_id, int64_t* count), (override));

    // Search
    MOCK_METHOD(int, search_fulltext, (const std::string& query, int top_k, std::vector<SearchResult>& results), (override));
    MOCK_METHOD(int, search_metadata, (const std::string& json_query, int top_k, std::vector<SearchResult>& results), (override));
};

}  // namespace testing
}  // namespace cortrix
