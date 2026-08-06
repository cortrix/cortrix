#include <gtest/gtest.h>
#include "cortrix/query/query_request.h"
#include "cortrix/query/query_response.h"

namespace cortrix {
namespace {

// --- FromJson tests ---

TEST(QueryRequestTest, FromJson_Complete) {
    json j = {
        {"query", "find risk documents"},
        {"namespace", "default"},
        {"top_k", 5},
        {"timeout_ms", 3000},
        {"filters", {
            {"block_type", {"FILE", "DATABASE"}},
            {"source_path", "/contracts/*"},
            {"created_after", "2025-01-01"}
        }},
        {"search_config", {
            {"enable_vector", true},
            {"enable_bm25", false},
            {"enable_sql", false}
        }}
    };

    QueryRequest req;
    Status s = QueryRequest::FromJson(j, &req);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(req.query, "find risk documents");
    EXPECT_EQ(req.namespace_name, "default");
    EXPECT_EQ(req.top_k, 5);
    EXPECT_EQ(req.timeout_ms, 3000);
    ASSERT_EQ(req.filters.block_types.size(), 2u);
    EXPECT_EQ(req.filters.block_types[0], "FILE");
    EXPECT_EQ(req.filters.block_types[1], "DATABASE");
    EXPECT_EQ(req.filters.source_path, "/contracts/*");
    EXPECT_EQ(req.filters.created_after, "2025-01-01");
    EXPECT_TRUE(req.search_config.enable_vector);
    EXPECT_FALSE(req.search_config.enable_bm25);
    EXPECT_FALSE(req.search_config.enable_sql);
}

TEST(QueryRequestTest, FromJson_MinimalRequired) {
    json j = {{"query", "test"}, {"namespace", "ns1"}};
    QueryRequest req;
    Status s = QueryRequest::FromJson(j, &req);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(req.query, "test");
    EXPECT_EQ(req.namespace_name, "ns1");
    // Defaults
    EXPECT_EQ(req.top_k, 10);
    EXPECT_EQ(req.timeout_ms, 5000);
    EXPECT_TRUE(req.search_config.enable_vector);
    EXPECT_TRUE(req.search_config.enable_bm25);
    EXPECT_TRUE(req.search_config.enable_sql);
}

TEST(QueryRequestTest, FromJson_NotObject) {
    json j = "not an object";
    QueryRequest req;
    Status s = QueryRequest::FromJson(j, &req);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// --- Validate tests ---

TEST(QueryRequestTest, Validate_Ok) {
    QueryRequest req;
    req.query = "test query";
    req.namespace_name = "default";
    req.top_k = 10;
    req.timeout_ms = 5000;
    EXPECT_TRUE(req.Validate().ok());
}

TEST(QueryRequestTest, Validate_EmptyQuery) {
    QueryRequest req;
    req.namespace_name = "default";
    Status s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "query is required");
}

// Design spec (boundary #13): queries > max_query_length are TRUNCATED, not rejected.
// Normalize() must be called before Validate() to apply truncation.
TEST(QueryRequestTest, Normalize_TruncatesLongQuery) {
    QueryRequest req;
    req.query = std::string(5000, 'x');  // Exceeds kMaxQueryLength (2000)
    req.namespace_name = "default";

    bool truncated = req.Normalize();
    EXPECT_TRUE(truncated);
    EXPECT_TRUE(req.was_truncated);
    EXPECT_EQ(req.query.size(), QueryRequest::kMaxQueryLength);

    // After truncation, Validate should pass
    EXPECT_TRUE(req.Validate().ok());
}

TEST(QueryRequestTest, Normalize_NoTruncationForShortQuery) {
    QueryRequest req;
    req.query = "short query";
    req.namespace_name = "default";

    bool truncated = req.Normalize();
    EXPECT_FALSE(truncated);
    EXPECT_FALSE(req.was_truncated);
    EXPECT_EQ(req.query, "short query");
}

TEST(QueryRequestTest, Normalize_ExactlyAtLimit) {
    QueryRequest req;
    req.query = std::string(QueryRequest::kMaxQueryLength, 'x');
    req.namespace_name = "default";

    bool truncated = req.Normalize();
    EXPECT_FALSE(truncated);
    EXPECT_EQ(req.query.size(), QueryRequest::kMaxQueryLength);
}

TEST(QueryRequestTest, Normalize_OneBeyondLimit) {
    QueryRequest req;
    req.query = std::string(QueryRequest::kMaxQueryLength + 1, 'x');
    req.namespace_name = "default";

    bool truncated = req.Normalize();
    EXPECT_TRUE(truncated);
    EXPECT_EQ(req.query.size(), QueryRequest::kMaxQueryLength);
}

TEST(QueryRequestTest, Normalize_VeryLongQuery10000) {
    // Design boundary #13: >10000 chars scenario
    QueryRequest req;
    req.query = std::string(10001, 'x');
    req.namespace_name = "default";

    bool truncated = req.Normalize();
    EXPECT_TRUE(truncated);
    EXPECT_EQ(req.query.size(), QueryRequest::kMaxQueryLength);  // truncated to 2000, not 10000
    EXPECT_TRUE(req.Validate().ok());
}

TEST(QueryRequestTest, Validate_AfterNormalize_LongQueryPasses) {
    // Previously this would fail with "query exceeds max length 10000"
    // After the design-alignment fix, Normalize truncates and Validate passes.
    QueryRequest req;
    req.query = std::string(10001, 'x');
    req.namespace_name = "default";
    req.Normalize();
    EXPECT_TRUE(req.Validate().ok());
}

TEST(QueryRequestTest, Validate_EmptyNamespace) {
    QueryRequest req;
    req.query = "test";
    Status s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "namespace is required");
}

TEST(QueryRequestTest, Validate_InvalidNamespaceFormat) {
    QueryRequest req;
    req.query = "test";
    req.namespace_name = "invalid namespace!";
    Status s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "invalid namespace format");
}

TEST(QueryRequestTest, Validate_NamespaceTooLong) {
    QueryRequest req;
    req.query = "test";
    req.namespace_name = std::string(65, 'a');
    Status s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "invalid namespace format");
}

TEST(QueryRequestTest, Validate_TopKZero) {
    QueryRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.top_k = 0;
    Status s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "top_k must be between 1 and 100");
}

TEST(QueryRequestTest, Validate_TopKTooHigh) {
    QueryRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.top_k = 101;
    Status s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "top_k must be between 1 and 100");
}

TEST(QueryRequestTest, Validate_TimeoutTooLow) {
    QueryRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.timeout_ms = 50;
    Status s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "timeout_ms must be between 100 and 30000");
}

TEST(QueryRequestTest, Validate_TimeoutTooHigh) {
    QueryRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.timeout_ms = 50000;
    Status s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "timeout_ms must be between 100 and 30000");
}

TEST(QueryRequestTest, Validate_InvalidBlockType) {
    QueryRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.filters.block_types = {"FILE", "INVALID_TYPE"};
    Status s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "invalid block_type: INVALID_TYPE");
}

TEST(QueryRequestTest, Validate_ValidBlockTypes) {
    QueryRequest req;
    req.query = "test";
    req.namespace_name = "default";
    req.filters.block_types = {"FILE", "DATABASE", "SCAN", "AUDIO", "VIDEO", "IMAGE", "MEMORY"};
    EXPECT_TRUE(req.Validate().ok());
}

// --- kMaxQueryLength constant ---

TEST(QueryRequestTest, MaxQueryLength_DefaultIs2000) {
    // Design spec: max_query_length config default = 2000
    EXPECT_EQ(QueryRequest::kMaxQueryLength, 2000u);
}

// --- BlockTypeFromString / BlockTypeToString ---

TEST(BlockTypeTest, FromString) {
    EXPECT_EQ(BlockTypeFromString("FILE"), 1);
    EXPECT_EQ(BlockTypeFromString("SCAN"), 2);
    EXPECT_EQ(BlockTypeFromString("DATABASE"), 3);
    EXPECT_EQ(BlockTypeFromString("AUDIO"), 4);
    EXPECT_EQ(BlockTypeFromString("VIDEO"), 5);
    EXPECT_EQ(BlockTypeFromString("IMAGE"), 6);
    EXPECT_EQ(BlockTypeFromString("MEMORY"), 7);
    EXPECT_EQ(BlockTypeFromString("INVALID"), -1);
}

TEST(BlockTypeTest, ToString) {
    EXPECT_EQ(BlockTypeToString(1), "FILE");
    EXPECT_EQ(BlockTypeToString(3), "DATABASE");
    EXPECT_EQ(BlockTypeToString(7), "MEMORY");
    EXPECT_EQ(BlockTypeToString(99), "UNKNOWN");
}

// --- QueryResponse ToJson ---

TEST(QueryResponseTest, ToJson_EmptyResults) {
    QueryResponse resp;
    resp.meta.total_results = 0;
    resp.meta.latency_ms = 10;
    resp.meta.routes_used = {"vector", "bm25"};
    resp.meta.degraded = false;
    resp.meta.degradation_level = 0;
    resp.sql_result.has_result = false;

    json j = resp.ToJson();
    EXPECT_TRUE(j["results"].is_array());
    EXPECT_TRUE(j["results"].empty());
    EXPECT_TRUE(j["sql_result"].is_null());
    EXPECT_EQ(j["meta"]["total_results"], 0);
    EXPECT_EQ(j["meta"]["latency_ms"], 10);
    EXPECT_FALSE(j["meta"]["degraded"]);
}

TEST(QueryResponseTest, ToJson_WithResults) {
    QueryResponse resp;
    ResultItem item;
    item.block_id = 42;
    item.doc_id = "test-doc-7";
    item.score = 0.03252f;
    item.source_path = "/test/doc.pdf";
    item.block_type = "FILE";
    item.chunk_text = "test content";
    item.metadata = json::object();
    item.related_blocks_count = 2;
    resp.results.push_back(item);

    resp.meta.total_results = 1;
    resp.meta.latency_ms = 120;
    resp.meta.routes_used = {"vector", "bm25"};
    resp.meta.degraded = false;
    resp.meta.degradation_level = 0;
    resp.sql_result.has_result = false;

    json j = resp.ToJson();
    ASSERT_EQ(j["results"].size(), 1u);
    EXPECT_EQ(j["results"][0]["block_id"], 42);
    EXPECT_EQ(j["results"][0]["doc_id"], "test-doc-7");
    EXPECT_EQ(j["results"][0]["source_path"], "/test/doc.pdf");
    EXPECT_EQ(j["results"][0]["block_type"], "FILE");
    EXPECT_EQ(j["results"][0]["related_blocks_count"], 2);
}

TEST(QueryResponseTest, ToJson_Degraded) {
    QueryResponse resp;
    resp.meta.total_results = 3;
    resp.meta.latency_ms = 4800;
    resp.meta.routes_used = {"bm25"};
    resp.meta.degraded = true;
    resp.meta.degraded_routes = {"vector"};
    resp.meta.degradation_level = 2;
    resp.sql_result.has_result = false;

    json j = resp.ToJson();
    EXPECT_TRUE(j["meta"]["degraded"]);
    EXPECT_EQ(j["meta"]["degradation_level"], 2);
    ASSERT_EQ(j["meta"]["degraded_routes"].size(), 1u);
    EXPECT_EQ(j["meta"]["degraded_routes"][0], "vector");
}

// Coverage: lines 26-41 of query_response.cpp (sql_result.has_result=true branch)
TEST(QueryResponseTest, ToJson_WithSqlResult) {
    QueryResponse resp;
    resp.meta.total_results = 0;
    resp.meta.latency_ms = 50;
    resp.meta.routes_used = {"sql"};
    resp.meta.degraded = false;
    resp.meta.degradation_level = 0;

    // Fill sql_result with has_result=true
    resp.sql_result.has_result = true;
    resp.sql_result.query = "SELECT * FROM blocks WHERE doc_id=1";
    resp.sql_result.columns = {"block_id", "content"};
    resp.sql_result.rows = {
        {json(1), json("first block text")},
        {json(2), json("second block text")}
    };
    resp.sql_result.row_count = 2;
    resp.sql_result.truncated = false;
    resp.sql_result.sql_latency_ms = 15;
    // error is empty -> should NOT appear in output

    json j = resp.ToJson();

    ASSERT_TRUE(j.contains("sql_result"));
    ASSERT_FALSE(j["sql_result"].is_null());
    EXPECT_EQ(j["sql_result"]["query"], "SELECT * FROM blocks WHERE doc_id=1");
    ASSERT_EQ(j["sql_result"]["columns"].size(), 2u);
    EXPECT_EQ(j["sql_result"]["columns"][0], "block_id");
    EXPECT_EQ(j["sql_result"]["columns"][1], "content");
    ASSERT_EQ(j["sql_result"]["rows"].size(), 2u);
    EXPECT_EQ(j["sql_result"]["row_count"], 2);
    EXPECT_FALSE(j["sql_result"]["truncated"].get<bool>());
    EXPECT_EQ(j["sql_result"]["sql_latency_ms"], 15);
    // When error is empty, "error" key should NOT be present
    EXPECT_FALSE(j["sql_result"].contains("error"));
}

// Coverage: line 37 of query_response.cpp (sql_result.error non-empty branch)
TEST(QueryResponseTest, ToJson_WithSqlResultError) {
    QueryResponse resp;
    resp.meta.total_results = 0;
    resp.meta.latency_ms = 100;
    resp.meta.routes_used = {"sql"};
    resp.meta.degraded = false;
    resp.meta.degradation_level = 0;

    resp.sql_result.has_result = true;
    resp.sql_result.query = "SELECT * FROM nonexistent";
    resp.sql_result.columns = {};
    resp.sql_result.rows = {};
    resp.sql_result.row_count = 0;
    resp.sql_result.truncated = false;
    resp.sql_result.error = "no such table: nonexistent";
    resp.sql_result.sql_latency_ms = 5;

    json j = resp.ToJson();

    ASSERT_TRUE(j.contains("sql_result"));
    ASSERT_FALSE(j["sql_result"].is_null());
    EXPECT_EQ(j["sql_result"]["error"], "no such table: nonexistent");
    EXPECT_EQ(j["sql_result"]["row_count"], 0);
}

// Coverage: lines 57-58 of query_response.cpp (has_error=true branch)
TEST(QueryResponseTest, ToJson_WithError) {
    QueryResponse resp;
    resp.meta.total_results = 0;
    resp.meta.latency_ms = 4999;
    resp.meta.routes_used = {};
    resp.meta.degraded = true;
    resp.meta.degraded_routes = {"vector", "bm25"};
    resp.meta.degradation_level = 3;
    resp.sql_result.has_result = false;
    resp.has_error = true;
    resp.error_message = "All routes failed";

    json j = resp.ToJson();

    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"], "All routes failed");
    EXPECT_TRUE(j["sql_result"].is_null());
    EXPECT_EQ(j["meta"]["degradation_level"], 3);
}

// Coverage: combined sql_result + error
TEST(QueryResponseTest, ToJson_WithSqlResultTruncated) {
    QueryResponse resp;
    resp.meta.total_results = 5;
    resp.meta.latency_ms = 200;
    resp.meta.routes_used = {"sql", "bm25"};
    resp.meta.degraded = false;
    resp.meta.degradation_level = 0;

    resp.sql_result.has_result = true;
    resp.sql_result.query = "SELECT * FROM blocks";
    resp.sql_result.columns = {"id"};
    resp.sql_result.rows = {{json(1)}, {json(2)}, {json(3)}};
    resp.sql_result.row_count = 100;  // more than returned
    resp.sql_result.truncated = true;
    resp.sql_result.sql_latency_ms = 42;

    json j = resp.ToJson();
    EXPECT_TRUE(j["sql_result"]["truncated"].get<bool>());
    EXPECT_EQ(j["sql_result"]["row_count"], 100);
    ASSERT_EQ(j["sql_result"]["rows"].size(), 3u);
}

// ---- doc_id filter parsing ----

TEST(QueryRequestTest, DocIdFilter_Parsed) {
    json j = {
        {"query", "test"},
        {"namespace", "default"},
        {"filters", {{"doc_id", "test-doc-42"}}}
    };
    QueryRequest req;
    auto s = QueryRequest::FromJson(j, &req);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(req.filters.doc_id, "test-doc-42");
}

TEST(QueryRequestTest, DocIdFilter_Zero_WhenAbsent) {
    json j = {{"query", "test"}, {"namespace", "default"}};
    QueryRequest req;
    QueryRequest::FromJson(j, &req);
    EXPECT_TRUE(req.filters.doc_id.empty());
}

TEST(QueryRequestTest, DocIdFilter_IgnoredIfNotString) {
    // D-I6: doc_id is a ULID string; a non-string value (e.g. an integer) is not
    // a valid doc_id filter and is ignored (left empty).
    json j = {
        {"query", "test"},
        {"namespace", "default"},
        {"filters", {{"doc_id", 42}}}
    };
    QueryRequest req;
    auto s = QueryRequest::FromJson(j, &req);
    EXPECT_TRUE(s.ok());
    EXPECT_TRUE(req.filters.doc_id.empty());  // non-string -> not set
}

}  // namespace
}  // namespace cortrix
