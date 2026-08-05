#include <gtest/gtest.h>

#include <functional>
#include <string>

#include "cortrix/common/status.h"
#include "cortrix/memory/memory_searcher.h"
#include "cortrix/query/query_request.h"

// Exhaustive field-by-field validation sweeps over the two request structs that
// expose a Validate() surface: MemorySearchRequest and QueryRequest.
// These are EXHAUSTIVE matrices in NEW suites (globally unique tokens
// SearcherValidateMatrix* / QueryReqValidateMatrix* / QueryReqNormalizeMatrix*)
// — the basic happy/sad tests live elsewhere; here we parameterize each field at
// {valid, empty, out-of-range-low, out-of-range-high, wrong-combination} against
// the ACTUAL bound constants read from the headers/impl:
//   MemorySearchRequest: top_k in [1,100], user_id ASCII printable len<=128,
//     session_id required iff scope==kSession.
//   QueryRequest: top_k in [1,100], timeout_ms in [100,30000], namespace
//     regex ^[a-zA-Z0-9_-]{1,64}$, block/exclude types from the fixed enum.

namespace cortrix {
namespace {

// A baseline-valid MemorySearchRequest the matrices mutate one field at a time.
MemorySearchRequest ValidMemReq() {
    MemorySearchRequest r;
    r.query = "hello";
    r.namespace_name = "ns1";
    r.user_id = "u1";
    r.scope = MemoryScope::kUser;
    r.top_k = 10;
    return r;
}

// ===========================================================================
// MemorySearchRequest::Validate — per-field matrix
// ===========================================================================

struct SearcherValidateCase {
    std::string name;
    // mutator applied to a fresh ValidMemReq()
    std::function<void(MemorySearchRequest&)> mutate;
    bool expect_ok;
};

class SearcherValidateMatrix
    : public ::testing::TestWithParam<SearcherValidateCase> {};

TEST_P(SearcherValidateMatrix, Field) {
    const auto& c = GetParam();
    MemorySearchRequest r = ValidMemReq();
    c.mutate(r);
    Status s = r.Validate();
    EXPECT_EQ(s.ok(), c.expect_ok) << c.name << " msg=" << s.message();
    if (!c.expect_ok) {
        EXPECT_EQ(s.code(), StatusCode::kInvalidArgument) << c.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Mem, SearcherValidateMatrix,
    ::testing::Values(
        SearcherValidateCase{"baseline_valid", [](MemorySearchRequest&) {}, true},
        // query
        SearcherValidateCase{"query_empty", [](MemorySearchRequest& r) { r.query = ""; }, false},
        SearcherValidateCase{"query_whitespace_ok", [](MemorySearchRequest& r) { r.query = " "; }, true},
        // namespace
        SearcherValidateCase{"ns_empty", [](MemorySearchRequest& r) { r.namespace_name = ""; }, false},
        // user_id required + format
        SearcherValidateCase{"user_empty", [](MemorySearchRequest& r) { r.user_id = ""; }, false},
        SearcherValidateCase{"user_len_128_ok", [](MemorySearchRequest& r) { r.user_id = std::string(128, 'a'); }, true},
        SearcherValidateCase{"user_len_129_bad", [](MemorySearchRequest& r) { r.user_id = std::string(129, 'a'); }, false},
        SearcherValidateCase{"user_control_char_bad", [](MemorySearchRequest& r) { r.user_id = std::string("u\x01"); }, false},
        SearcherValidateCase{"user_high_byte_bad", [](MemorySearchRequest& r) { r.user_id = std::string("u\x7F"); }, false},
        SearcherValidateCase{"user_printable_space_ok", [](MemorySearchRequest& r) { r.user_id = "user one"; }, true},
        SearcherValidateCase{"user_tilde_ok", [](MemorySearchRequest& r) { r.user_id = "u~"; }, true},
        // scope / session_id combination
        SearcherValidateCase{"session_scope_missing_sid_bad",
            [](MemorySearchRequest& r) { r.scope = MemoryScope::kSession; r.session_id = ""; }, false},
        SearcherValidateCase{"session_scope_with_sid_ok",
            [](MemorySearchRequest& r) { r.scope = MemoryScope::kSession; r.session_id = "s1"; }, true},
        SearcherValidateCase{"user_scope_ignores_missing_sid",
            [](MemorySearchRequest& r) { r.scope = MemoryScope::kUser; r.session_id = ""; }, true},
        // top_k bounds [1,100]
        SearcherValidateCase{"topk_low_0", [](MemorySearchRequest& r) { r.top_k = 0; }, false},
        SearcherValidateCase{"topk_neg", [](MemorySearchRequest& r) { r.top_k = -5; }, false},
        SearcherValidateCase{"topk_min_1_ok", [](MemorySearchRequest& r) { r.top_k = 1; }, true},
        SearcherValidateCase{"topk_max_100_ok", [](MemorySearchRequest& r) { r.top_k = 100; }, true},
        SearcherValidateCase{"topk_high_101", [](MemorySearchRequest& r) { r.top_k = 101; }, false}),
    [](const ::testing::TestParamInfo<SearcherValidateCase>& i) { return i.param.name; });

// ===========================================================================
// QueryRequest::Validate — per-field matrix
// ===========================================================================

QueryRequest ValidQueryReq() {
    QueryRequest q;
    q.query = "find docs";
    q.namespace_name = "ns_1";
    q.top_k = 10;
    q.timeout_ms = 5000;
    return q;
}

struct QueryReqValidateCase {
    std::string name;
    std::function<void(QueryRequest&)> mutate;
    bool expect_ok;
};

class QueryReqValidateMatrix
    : public ::testing::TestWithParam<QueryReqValidateCase> {};

TEST_P(QueryReqValidateMatrix, Field) {
    const auto& c = GetParam();
    QueryRequest q = ValidQueryReq();
    c.mutate(q);
    Status s = q.Validate();
    EXPECT_EQ(s.ok(), c.expect_ok) << c.name << " msg=" << s.message();
    if (!c.expect_ok) {
        EXPECT_EQ(s.code(), StatusCode::kInvalidArgument) << c.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Query, QueryReqValidateMatrix,
    ::testing::Values(
        QueryReqValidateCase{"baseline_valid", [](QueryRequest&) {}, true},
        // query required
        QueryReqValidateCase{"query_empty", [](QueryRequest& q) { q.query = ""; }, false},
        // namespace regex ^[a-zA-Z0-9_-]{1,64}$
        QueryReqValidateCase{"ns_empty", [](QueryRequest& q) { q.namespace_name = ""; }, false},
        QueryReqValidateCase{"ns_len_64_ok", [](QueryRequest& q) { q.namespace_name = std::string(64, 'a'); }, true},
        QueryReqValidateCase{"ns_len_65_bad", [](QueryRequest& q) { q.namespace_name = std::string(65, 'a'); }, false},
        QueryReqValidateCase{"ns_dash_underscore_ok", [](QueryRequest& q) { q.namespace_name = "a-b_c"; }, true},
        QueryReqValidateCase{"ns_space_bad", [](QueryRequest& q) { q.namespace_name = "a b"; }, false},
        QueryReqValidateCase{"ns_slash_bad", [](QueryRequest& q) { q.namespace_name = "a/b"; }, false},
        QueryReqValidateCase{"ns_dot_bad", [](QueryRequest& q) { q.namespace_name = "a.b"; }, false},
        // top_k [1,100]
        QueryReqValidateCase{"topk_0_bad", [](QueryRequest& q) { q.top_k = 0; }, false},
        QueryReqValidateCase{"topk_1_ok", [](QueryRequest& q) { q.top_k = 1; }, true},
        QueryReqValidateCase{"topk_100_ok", [](QueryRequest& q) { q.top_k = 100; }, true},
        QueryReqValidateCase{"topk_101_bad", [](QueryRequest& q) { q.top_k = 101; }, false},
        // timeout_ms [100,30000]
        QueryReqValidateCase{"timeout_99_bad", [](QueryRequest& q) { q.timeout_ms = 99; }, false},
        QueryReqValidateCase{"timeout_100_ok", [](QueryRequest& q) { q.timeout_ms = 100; }, true},
        QueryReqValidateCase{"timeout_30000_ok", [](QueryRequest& q) { q.timeout_ms = 30000; }, true},
        QueryReqValidateCase{"timeout_30001_bad", [](QueryRequest& q) { q.timeout_ms = 30001; }, false},
        QueryReqValidateCase{"timeout_zero_bad", [](QueryRequest& q) { q.timeout_ms = 0; }, false},
        // block_types: valid names from BlockTypeFromString
        QueryReqValidateCase{"block_type_FILE_ok", [](QueryRequest& q) { q.filters.block_types = {"FILE"}; }, true},
        QueryReqValidateCase{"block_type_MEMORY_ok", [](QueryRequest& q) { q.filters.block_types = {"MEMORY"}; }, true},
        QueryReqValidateCase{"block_type_unknown_bad", [](QueryRequest& q) { q.filters.block_types = {"BOGUS"}; }, false},
        QueryReqValidateCase{"block_type_lowercase_bad", [](QueryRequest& q) { q.filters.block_types = {"file"}; }, false},
        QueryReqValidateCase{"block_type_mixed_one_bad", [](QueryRequest& q) { q.filters.block_types = {"FILE", "NOPE"}; }, false},
        // exclude_types
        QueryReqValidateCase{"exclude_type_ok", [](QueryRequest& q) { q.filters.exclude_types = {"SCAN"}; }, true},
        QueryReqValidateCase{"exclude_type_bad", [](QueryRequest& q) { q.filters.exclude_types = {"XYZ"}; }, false}),
    [](const ::testing::TestParamInfo<QueryReqValidateCase>& i) { return i.param.name; });

// ===========================================================================
// QueryRequest::Normalize — truncation boundary matrix (kMaxQueryLength=2000)
// ===========================================================================

struct QueryReqNormalizeCase {
    std::string name;
    size_t in_len;
    bool expect_truncated;
    size_t expect_out_len;
};

class QueryReqNormalizeMatrix
    : public ::testing::TestWithParam<QueryReqNormalizeCase> {};

TEST_P(QueryReqNormalizeMatrix, Truncate) {
    const auto& c = GetParam();
    QueryRequest q;
    q.namespace_name = "ns";
    q.query = std::string(c.in_len, 'x');
    bool truncated = q.Normalize();
    EXPECT_EQ(truncated, c.expect_truncated) << c.name;
    EXPECT_EQ(q.was_truncated, c.expect_truncated) << c.name;
    EXPECT_EQ(q.query.size(), c.expect_out_len) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Norm, QueryReqNormalizeMatrix,
    ::testing::Values(
        QueryReqNormalizeCase{"empty_no_trunc", 0, false, 0},
        QueryReqNormalizeCase{"short_no_trunc", 100, false, 100},
        QueryReqNormalizeCase{"at_limit_no_trunc", QueryRequest::kMaxQueryLength, false, QueryRequest::kMaxQueryLength},
        QueryReqNormalizeCase{"one_over_trunc", QueryRequest::kMaxQueryLength + 1, true, QueryRequest::kMaxQueryLength},
        QueryReqNormalizeCase{"far_over_trunc", QueryRequest::kMaxQueryLength * 5, true, QueryRequest::kMaxQueryLength}),
    [](const ::testing::TestParamInfo<QueryReqNormalizeCase>& i) { return i.param.name; });

}  // namespace
}  // namespace cortrix
