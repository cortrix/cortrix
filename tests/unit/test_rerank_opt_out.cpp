// Issue #75 -- the per-request rerank opt-out is the only control that currently
// suppresses cross-encoder reranking, so duplicate-detection and similar-item
// workloads depend on it entirely.
//
// Reranking is on by default and, on that workload shape, is severely harmful:
// measured on BEIR Quora (522,927 documents, 2,000 queries) it costs 62% of nDCG@10
// and 59% of recall@10 versus dense retrieval alone, because a cross-encoder scores
// topical relevance while the ground truth is "same question". See
// docs/operations/reranking-applicability.md.
//
// The namespace-level setting (namespaces.reranker_config {"enabled": false}) is
// implemented and unit-tested in RerankerConfigResolver but is not consulted by the
// live query path, so it cannot serve as the mitigation today. That leaves the
// request flag carrying the whole burden, which is why its behaviour is pinned here
// rather than left to the parser's general request tests.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cortrix/query/cross_ns_query_handler.h"
#include "cortrix/query/cross_ns_request.h"
#include "cortrix/common/status.h"

namespace cortrix::query {
namespace {

nlohmann::json BaseBody() {
    return nlohmann::json{
        {"query", "What is the step by step guide to invest in share market in india?"},
        {"namespaces", nlohmann::json::array({"faq-dedup"})},
        {"top_k", 10},
    };
}

QueryRequest ParseOrFail(const nlohmann::json& body) {
    QueryRequest out;
    const Status s = CrossNsQueryHandler::ParseRequest(body, &out);
    EXPECT_TRUE(s.ok()) << s.message();
    return out;
}

// The default is what makes this issue bite: a caller that says nothing gets
// reranking, including on a workload where it removes the correct answer from the
// result set entirely. Changing the default is a compatibility decision; this case
// exists so such a change cannot happen silently.
TEST(RerankOptOutTest, RerankingIsOnWhenTheRequestSaysNothing) {
    EXPECT_TRUE(ParseOrFail(BaseBody()).rerank);
}

TEST(RerankOptOutTest, ExplicitFalseDisablesReranking) {
    nlohmann::json body = BaseBody();
    body["rerank"] = false;
    EXPECT_FALSE(ParseOrFail(body).rerank);
}

TEST(RerankOptOutTest, ExplicitTrueKeepsReranking) {
    nlohmann::json body = BaseBody();
    body["rerank"] = true;
    EXPECT_TRUE(ParseOrFail(body).rerank);
}

// A dedup caller that sends the flag in the wrong shape must not be silently
// downgraded into the default: the failure mode is invisible at the API boundary
// (results still come back, just reranked) so a wrong-typed value is worth pinning.
// Current behaviour is to ignore a non-boolean and keep the default -- recorded here
// so that if it ever becomes a validation error, the change is deliberate.
TEST(RerankOptOutTest, NonBooleanRerankFieldFallsBackToTheDefault) {
    for (const nlohmann::json& value : {nlohmann::json("false"), nlohmann::json(0),
                                        nlohmann::json(nlohmann::json::value_t::null)}) {
        nlohmann::json body = BaseBody();
        body["rerank"] = value;
        EXPECT_TRUE(ParseOrFail(body).rerank)
            << "rerank=" << value.dump() << " should not be read as an opt-out";
    }
}

// The opt-out must survive alongside the other per-request switches a dedup caller
// is likely to set at the same time (retrieval-path toggles), rather than being
// reset by later parsing of the same body.
TEST(RerankOptOutTest, OptOutSurvivesAlongsideSearchConfig) {
    nlohmann::json body = BaseBody();
    body["rerank"] = false;
    body["search_config"] = {{"enable_vector", true}, {"enable_bm25", false}};
    const QueryRequest parsed = ParseOrFail(body);
    EXPECT_FALSE(parsed.rerank);
    EXPECT_TRUE(parsed.search_config.enable_vector);
    EXPECT_FALSE(parsed.search_config.enable_bm25);
}


// top_k boundary contract (issue #87). cross_ns_request.h documents top_k as
// range 1-100; ParseRequest must enforce it at the request boundary so an
// out-of-range value is a 400, not silently accepted and carried into retrieval.
TEST(CrossNsTopKBoundTest, RejectsTopKAboveMax) {
    nlohmann::json body = BaseBody();
    body["top_k"] = 100000000;  // the unbounded value that drove the finding
    QueryRequest out;
    const Status s = CrossNsQueryHandler::ParseRequest(body, &out);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.message().find("top_k") != std::string::npos) << s.message();
}

TEST(CrossNsTopKBoundTest, RejectsTopKBelowMin) {
    nlohmann::json body = BaseBody();
    body["top_k"] = 0;
    QueryRequest out;
    EXPECT_FALSE(CrossNsQueryHandler::ParseRequest(body, &out).ok());
}

TEST(CrossNsTopKBoundTest, AcceptsTopKAtTheBoundaries) {
    for (int k : {1, 50, 100}) {
        nlohmann::json body = BaseBody();
        body["top_k"] = k;
        QueryRequest out;
        EXPECT_TRUE(CrossNsQueryHandler::ParseRequest(body, &out).ok())
            << "top_k=" << k << " should be accepted";
        EXPECT_EQ(out.top_k, k);
    }
}
}  // namespace
}  // namespace cortrix::query
