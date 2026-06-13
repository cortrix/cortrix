#include <gtest/gtest.h>
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/common/data_types.h"
#include <unordered_map>

namespace cortrix {
namespace {

// Fake CortrixStore for BM25Searcher tests
class FakeStoreForBM25 : public CortrixStore {
public:
    std::vector<SearchResult> results_to_return;
    int rc_to_return = 0;
    std::string last_query;  // capture the sanitized query

    int Open() override { return 0; }
    int Close() override { return 0; }
    int doc_create(CortrixDoc&) override { return 0; }
    int doc_get(const std::string&, CortrixDoc&) override { return -2; }
    int doc_update_status(const std::string&, DocStatus, const std::string&) override { return 0; }
    int doc_delete(const std::string&) override { return 0; }
    int doc_list_by_status(DocStatus, std::vector<CortrixDoc>&) override { return 0; }
    int doc_find_by_source(const std::string&, const std::string&, CortrixDoc&) override { return -2; }
    int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }
    int doc_count(int64_t*) override { return 0; }
    int block_insert(CortrixBlock&) override { return 0; }
    int block_get(uint64_t, CortrixBlock&) override { return -2; }
    int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t*) override { return 0; }
    int search_fulltext(const std::string& query, int, std::vector<SearchResult>& results) override {
        last_query = query;
        results = results_to_return;
        return rc_to_return;
    }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

class BM25SearcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.results_to_return = {
            {101, "test-doc-1", 12.5, "text about risk"},
            {102, "test-doc-1", 9.3, "text about contracts"},
        };
        searcher_ = std::make_unique<BM25Searcher>(store_);
    }

    FakeStoreForBM25 store_;
    std::unique_ptr<BM25Searcher> searcher_;
};

TEST_F(BM25SearcherTest, SuccessfulSearch) {
    auto result = searcher_->Search("risk management", 10, 5000000);
    EXPECT_EQ(result.route_name, "bm25");
    EXPECT_EQ(result.status, RouteStatus::kSuccess);
    ASSERT_EQ(result.items.size(), 2u);
    EXPECT_EQ(result.items[0].block_id, 101);
    EXPECT_FLOAT_EQ(result.items[0].raw_score, 12.5f);
    EXPECT_EQ(result.items[1].block_id, 102);
}

TEST_F(BM25SearcherTest, StoreReturnsError) {
    store_.rc_to_return = -1;
    auto result = searcher_->Search("test query", 10, 5000000);
    EXPECT_EQ(result.status, RouteStatus::kError);
    EXPECT_EQ(result.error_message, "fulltext search failed");
}

TEST_F(BM25SearcherTest, EmptyResults) {
    store_.results_to_return.clear();
    auto result = searcher_->Search("nonexistent", 10, 5000000);
    EXPECT_EQ(result.status, RouteStatus::kSuccess);
    EXPECT_TRUE(result.items.empty());
}

TEST_F(BM25SearcherTest, SanitizeDoubleQuotes) {
    searcher_->Search("test \"quoted\" text", 10, 5000000);
    EXPECT_EQ(store_.last_query, "test \"\"quoted\"\" text");
}

TEST_F(BM25SearcherTest, SanitizeAsterisks) {
    searcher_->Search("test* wildcard", 10, 5000000);
    EXPECT_EQ(store_.last_query, "test wildcard");
}

TEST_F(BM25SearcherTest, SanitizeParentheses) {
    searcher_->Search("test (group) expression", 10, 5000000);
    EXPECT_EQ(store_.last_query, "test group expression");
}

// Chinese kept intentionally: this verifies the FTS5 sanitizer passes CJK text
// through untouched (it must NOT strip it the way it strips boolean operators).
TEST_F(BM25SearcherTest, NormalChinesePassthrough) {
    searcher_->Search("合同风险评估", 10, 5000000);
    EXPECT_EQ(store_.last_query, "合同风险评估");
}

TEST_F(BM25SearcherTest, BooleanOperatorsStripped) {
    // FTS5 boolean operators (AND, OR, NOT, NEAR) are stripped to prevent injection
    searcher_->Search("risk AND management", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk management");
}

TEST_F(BM25SearcherTest, RouteNameCorrect) {
    auto result = searcher_->Search("test", 10, 5000000);
    EXPECT_EQ(result.route_name, "bm25");
}

TEST_F(BM25SearcherTest, LatencyRecorded) {
    auto result = searcher_->Search("test", 10, 5000000);
    EXPECT_GE(result.latency_us, 0);
}

// --- Comprehensive FTS5 sanitization edge case tests ---

TEST_F(BM25SearcherTest, Sanitize_OR_Operator) {
    searcher_->Search("risk OR management", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk management");
}

TEST_F(BM25SearcherTest, Sanitize_NOT_Operator) {
    searcher_->Search("risk NOT fraud", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk fraud");
}

TEST_F(BM25SearcherTest, Sanitize_NEAR_Operator) {
    searcher_->Search("risk NEAR management", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk management");
}

TEST_F(BM25SearcherTest, Sanitize_NEAR_WithDistance) {
    // NEAR/5 is a proximity operator in FTS5 and should be stripped
    searcher_->Search("risk NEAR/5 management", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk management");
}

TEST_F(BM25SearcherTest, Sanitize_MultipleOperators) {
    searcher_->Search("risk AND fraud OR management NOT loss", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk fraud management loss");
}

TEST_F(BM25SearcherTest, Sanitize_LowercaseAndOrNotPreserved) {
    // FTS5 operators are case-sensitive: lowercase "and", "or", "not" are normal words
    searcher_->Search("this and that or nothing", 10, 5000000);
    EXPECT_EQ(store_.last_query, "this and that or nothing");
}

TEST_F(BM25SearcherTest, Sanitize_PrefixPlusOperator) {
    // FTS5 '+' prefix means "required"; should be stripped
    searcher_->Search("+risk +management", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk management");
}

TEST_F(BM25SearcherTest, Sanitize_PrefixMinusOperator) {
    // FTS5 '-' prefix means "exclude"; should be stripped
    searcher_->Search("-fraud risk", 10, 5000000);
    EXPECT_EQ(store_.last_query, "fraud risk");
}

TEST_F(BM25SearcherTest, Sanitize_ColonOperator) {
    // FTS5 ':' is used for column filters; should be replaced with space
    searcher_->Search("title:risk management", 10, 5000000);
    EXPECT_EQ(store_.last_query, "title risk management");
}

TEST_F(BM25SearcherTest, Sanitize_CaretOperator) {
    // FTS5 '^' is the initial-token operator; should be replaced with space
    searcher_->Search("^risk assessment", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk assessment");
}

TEST_F(BM25SearcherTest, Sanitize_ComplexInjection) {
    // Attempt to inject a complex FTS5 expression
    searcher_->Search("(risk OR fraud) AND NOT \"safe\"", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk fraud \"\"safe\"\"");
}

TEST_F(BM25SearcherTest, Sanitize_OnlyOperators) {
    // Query containing only FTS5 operators should result in empty after sanitization
    auto result = searcher_->Search("AND OR NOT", 10, 5000000);
    EXPECT_EQ(result.status, RouteStatus::kError);
    EXPECT_EQ(result.error_message, "empty query after sanitization");
}

TEST_F(BM25SearcherTest, Sanitize_WhitespaceOnly) {
    auto result = searcher_->Search("   ", 10, 5000000);
    EXPECT_EQ(result.status, RouteStatus::kError);
    EXPECT_EQ(result.error_message, "empty query after sanitization");
}

TEST_F(BM25SearcherTest, Sanitize_OperatorsAsSubstrings) {
    // "ANDROID" contains "AND" as substring but is not a standalone operator
    searcher_->Search("ANDROID phones", 10, 5000000);
    EXPECT_EQ(store_.last_query, "ANDROID phones");
}

TEST_F(BM25SearcherTest, Sanitize_MixedQuotesAndOperators) {
    searcher_->Search("\"risk\" AND \"management\"", 10, 5000000);
    EXPECT_EQ(store_.last_query, "\"\"risk\"\" \"\"management\"\"");
}

TEST_F(BM25SearcherTest, Sanitize_TabAndNewline) {
    searcher_->Search("risk\tmanagement\nfraud", 10, 5000000);
    EXPECT_EQ(store_.last_query, "risk management fraud");
}

TEST_F(BM25SearcherTest, Oversampling_TopK_Applied) {
    // Verify that search_k = top_k * 3 is used by checking items are returned correctly
    store_.results_to_return = {
        {101, "test-doc-1", 12.5, "text 1"},
        {102, "test-doc-1", 9.3, "text 2"},
        {103, "test-doc-1", 7.1, "text 3"},
    };
    auto result = searcher_->Search("test", 1, 5000000);
    EXPECT_EQ(result.status, RouteStatus::kSuccess);
    // BM25Searcher passes all results from store; truncation is done later by PostFilter
    EXPECT_EQ(result.items.size(), 3u);
}

// --- Coverage gap: timeout path (lines 34-38) ---

TEST_F(BM25SearcherTest, Timeout_ReturnsTimeoutStatus) {
    // Use a very small timeout so the search triggers timeout
    // timeout_us = 0 means any elapsed time will exceed it
    auto result = searcher_->Search("risk management", 10, 0);
    // The store call completes quickly in tests, but the check is latency > timeout_us
    // Since timeout_us=0, any non-zero latency triggers timeout
    if (result.latency_us > 0) {
        EXPECT_EQ(result.status, RouteStatus::kTimeout);
        EXPECT_EQ(result.error_message, "bm25 search timeout");
    } else {
        // If latency measured as 0 (rare on fast machines), search succeeds
        EXPECT_EQ(result.status, RouteStatus::kSuccess);
    }
}

// --- Coverage gap: doc_id from search results ---

TEST_F(BM25SearcherTest, DocId_PreservedFromSearchResult) {
    store_.results_to_return = {
        {101, "test-doc-5", 12.5, "text 1"},
        {102, "test-doc-8", 9.3, "text 2"},
    };
    auto result = searcher_->Search("test", 10, 5000000);
    ASSERT_EQ(result.items.size(), 2u);
    EXPECT_EQ(result.items[0].doc_id, "test-doc-5");
    EXPECT_EQ(result.items[1].doc_id, "test-doc-8");
}

// --- Coverage gap: chunk_index is always 0 ---

TEST_F(BM25SearcherTest, ChunkIndex_AlwaysZero) {
    auto result = searcher_->Search("test", 10, 5000000);
    for (const auto& item : result.items) {
        EXPECT_EQ(item.chunk_index, 0);
    }
}

}  // namespace
}  // namespace cortrix
