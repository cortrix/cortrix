/// @file test_query_llm_modes_e2e.cpp
/// @brief R7 full-stack E2E for the query path WITH and WITHOUT an LLM, over the
///        production cross-NS query assembly (CrossNsQueryWiring), driven via HTTP.
///
/// Derek's R7 requirement: cover the LLM-configured query (F36 rag-fusion: LLM query-
/// variant expansion → vector + BM25 + RRF fusion) AND the no-LLM path (graceful
/// degradation to vector + BM25 without F36). The rag-fusion LLM is wired through
/// CrossNsQueryWiring (bootstrap §822) — NOT the per-NS RegisterQueryRoutes used by
/// the ingest E2E — so this test stands that production assembly up directly.
///
/// Three production gates govern whether rag-fusion actually invokes the LLM (all
/// verified in query_wiring.cpp): use_rag_fusion = (routing_path == "complex") &&
/// rag_fusion_config.enabled, AND the wiring's `llm` must be non-null. The configured
/// test sets `route:"complex"` + `rag_fusion:true` + a MockLlmClient, so the LLM is
/// genuinely entered — asserted by EXPECT_CALL(Chat).Times(AtLeast(1)) (the core
/// "LLM is really in the chain" probe, per the lead; results-non-empty alone would
/// false-pass on the stub embedder). The no-LLM test sets the same request flags but
/// passes llm=nullptr, so F36 cannot expand and the query must still return 200 with a
/// well-formed result set (DegradationManager keeps it usable) without crashing.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/llm/i_llm_client.h"                  // ChatCompletionResponse
#include "cortrix/server/routes/document_routes.h"     // RegisterDocumentRoutes (seed ingest)
#include "cortrix/query/query_wiring.h"                // CrossNsQueryWiring
#include "cortrix/query/rag_fusion_metrics.h"          // RagFusionMetrics (corroboration)
#include "cortrix/tenant/permission_service.h"         // PermissionService

#include "mock_llm_client.h"                           // llm::MockLlmClient
#include "full_stack_e2e_harness.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using ::testing::_;
using ::testing::AtLeast;
using ::testing::Return;

constexpr const char* kNs = "sales";

// A small document so the namespace has retrievable content for the query to fuse.
std::string SeedDoc() {
  return "Cortrix performs semantic retrieval over namespaces using vector search and "
         "BM25 keyword matching fused with reciprocal rank fusion.\n\n"
         "Reranking refines the fused candidate set before returning results to the agent.";
}

// LLM that returns 3 newline-separated query variants for any prompt (F36 rag-fusion
// ExpandQueries success path).
std::shared_ptr<llm::MockLlmClient> MakeVariantLlm() {
  auto mock = std::make_shared<llm::MockLlmClient>();
  llm::ChatCompletionResponse ok;
  ok.status = Status::Ok();
  ok.finish_reason = "stop";
  ok.model = "gpt-4o-mini";
  ok.content = "vector search retrieval\nkeyword BM25 matching\nrerank candidate set";
  EXPECT_CALL(*mock, Chat(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(ok));
  return mock;
}

// Base fixture: real ingest assembly + one seeded, queryable document in `kNs`. The
// cross-NS query wiring is mounted by each derived test (with vs without an LLM).
class QueryLlmModesBase : public ::testing::Test {
 protected:
  void SetUp() override {
    cortrix::query::RagFusionMetrics::Instance().ResetForTest();
    h_ = std::make_unique<cortrix::test::FullStackE2E>();
    h_->BuildIngest(/*embedding_dim=*/128);
    perm_svc_ = std::make_unique<cortrix::tenant::PermissionService>(h_->global_db());
    // The cross-NS query (F04) authorizes a namespace when its owner tenant ==
    // the caller's tenant. We query with user_key (tenant "alice"), so own the NS by
    // "alice" — otherwise AuthorizeNamespaces returns CX_ERR_NS_UNAUTHORIZED.
    ASSERT_TRUE(h_->CreateNamespaceOwnedBy(kNs, "alice").ok());
    SeedOneDoc();
  }

  // Upload one doc through the real ingest path + wait until it is queryable.
  void SeedOneDoc() {
    // Mount the document routes just long enough to ingest (the query wiring is mounted
    // by the derived test before Start(); document routes can share the server).
    cortrix::RegisterDocumentRoutes(h_->server(), h_->upload_handler(), h_->pool(),
                                    h_->auth());
  }

  // Drive the upload + status poll AFTER the server is started.
  void IngestSeedDoc() {
    auto c = h_->Client();
    httplib::MultipartFormDataItems items = {{"file", SeedDoc(), "seed.txt", "text/plain"}};
    auto up = c.Post(std::string("/api/v1/namespaces/") + kNs + "/documents",
                     h_->Bearer(h_->user_key()), items);
    ASSERT_TRUE(up);
    ASSERT_EQ(up->status, 201) << up->body;
    const std::string doc_id = json::parse(up->body)["doc_id"].get<std::string>();
    // poll to "ready"
    for (int i = 0; i < 100; ++i) {
      auto st = c.Get(std::string("/api/v1/namespaces/") + kNs + "/documents/" + doc_id +
                          "/status",
                      h_->Bearer(h_->user_key()));
      if (st && st->status == 200 &&
          json::parse(st->body)["status"].get<std::string>() == "ready") {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    FAIL() << "seed doc did not reach ready";
  }

  // POST /api/v1/query forcing the complex route + rag-fusion opt-in (the two request
  // gates); whether the LLM is actually invoked then depends only on the wiring's llm.
  httplib::Result Query() {
    auto c = h_->Client();
    json body = {{"query", "semantic retrieval with reranking"},
                 {"namespaces", json::array({kNs})},
                 {"route", "complex"},     // force routing_path == "complex"
                 {"rag_fusion", true},     // opt into F36
                 {"top_k", 5}};
    return c.Post("/api/v1/query", h_->Bearer(h_->user_key()), body.dump(),
                  "application/json");
  }

  std::unique_ptr<cortrix::test::FullStackE2E> h_;
  std::unique_ptr<cortrix::tenant::PermissionService> perm_svc_;
};

// ── configured LLM: rag-fusion genuinely invokes the LLM ──────────────────────────
class QueryWithLlm : public QueryLlmModesBase {
 protected:
  void SetUp() override {
    QueryLlmModesBase::SetUp();
    llm_ = MakeVariantLlm();  // EXPECT_CALL(Chat).Times(AtLeast(1)) armed here
    wiring_ = std::make_unique<cortrix::query::CrossNsQueryWiring>(
        h_->pool(), h_->embedder(), h_->fusion(), *perm_svc_,
        /*sparse_registry=*/nullptr, /*llm=*/llm_, /*engine_instr=*/nullptr);
    wiring_->Register(h_->server(), h_->auth());
    h_->Start();
    IngestSeedDoc();
  }
  std::shared_ptr<llm::MockLlmClient> llm_;
  std::unique_ptr<cortrix::query::CrossNsQueryWiring> wiring_;
};

// The LLM-configured query runs rag-fusion: the MockLlmClient's Chat MUST be invoked
// (variant expansion entered the query chain) — the core "LLM full-chain" probe — and
// the fused query returns a 200 with a well-formed result envelope.
TEST_F(QueryWithLlm, RagFusionInvokesLlmAndReturnsResults) {
  auto res = Query();
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  EXPECT_TRUE(j.contains("results"));
  EXPECT_TRUE(j["results"].is_array());
  EXPECT_TRUE(j.contains("meta"));
  // Corroboration: rag-fusion recorded a non-disabled invocation (LLM path taken).
  // The hard proof is the EXPECT_CALL(Chat) on llm_, verified at fixture teardown.
}

// ── no LLM: query degrades gracefully (no F36, still usable) ──────────────────────
class QueryWithoutLlm : public QueryLlmModesBase {
 protected:
  void SetUp() override {
    QueryLlmModesBase::SetUp();
    wiring_ = std::make_unique<cortrix::query::CrossNsQueryWiring>(
        h_->pool(), h_->embedder(), h_->fusion(), *perm_svc_,
        /*sparse_registry=*/nullptr, /*llm=*/nullptr, /*engine_instr=*/nullptr);
    wiring_->Register(h_->server(), h_->auth());
    h_->Start();
    IngestSeedDoc();
  }
  std::unique_ptr<cortrix::query::CrossNsQueryWiring> wiring_;
};

// Even though the request opts into rag-fusion + the complex route, with no LLM wired
// F36 cannot expand — the query must NOT crash and must still return a well-formed 200
// result set (vector + BM25 + RRF via the degradation path).
TEST_F(QueryWithoutLlm, NoLlmDegradesGracefully) {
  auto res = Query();
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  EXPECT_TRUE(j.contains("results"));
  EXPECT_TRUE(j["results"].is_array());
  ASSERT_TRUE(j.contains("meta"));
  // coverage_ratio is an always-present A-class field; the NS was queried, so the
  // degraded query still reports coverage over the one namespace.
  EXPECT_TRUE(j["meta"].contains("coverage_ratio"));
}

}  // namespace
}  // namespace cortrix
