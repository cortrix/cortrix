/// @file test_query_llm_modes_e2e.cpp
/// @brief R7 full-stack E2E for the query path WITH and WITHOUT an LLM, over the
///        production cross-NS query assembly (CrossNsQueryWiring), driven via HTTP.
///
/// Covers the LLM-configured query (rag-fusion: LLM query-
/// variant expansion → vector + BM25 + RRF fusion) AND the no-LLM path (graceful
/// degradation to vector + BM25 without RAG-Fusion). The rag-fusion LLM is wired through
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
/// passes llm=nullptr, so RAG-Fusion cannot expand and the query must still return 200 with a
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

#include "cortrix/auth/auth_middleware.h"              // WithAuth / kPermRead
#include "cortrix/doc_summary/discover_handler.h"      // HandleDocumentsDiscover
#include "cortrix/doc_summary/doc_summary_generator.h" // DocSummaryConfig
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
using ::testing::HasSubstr;
using ::testing::Return;

constexpr const char* kNs = "sales";

bool HasResultDocId(const json& body, const std::string& doc_id);
int ResultDocPosition(const json& body, const std::string& doc_id);

// Two small documents let the E2E prove RAG-Fusion variants affect final order, not just
// that Chat() was called. We drive `granularity=doc`, so matching is deterministic
// through META block metadata -> doc_fts5 rather than dependent on stub dense vectors:
// the original query matches the baseline doc metadata, while only the JSON LLM
// variants match the variant-only doc metadata.
std::string OriginalOnlyDoc() {
  return "F36 baseline document. The discriminating query token is stored only in "
         "upload metadata, not in this body.";
}

std::string VariantOnlyDoc() {
  return "F36 variant document. The discriminating query tokens are stored only in "
         "upload metadata, not in this body.";
}

std::string VariantLlmJson() {
  return R"({"variants":[
    {"strategy":"paraphrase","query":"zephyrmarker"},
    {"strategy":"subquery","query":"quasaranchor"},
    {"strategy":"reverse","query":"novatok"}
  ]})";
}

// LLM that returns strict JSON query variants for any prompt (rag-fusion
// ExpandQueries success path).
std::shared_ptr<llm::MockLlmClient> MakeVariantLlm() {
  auto mock = std::make_shared<llm::MockLlmClient>();
  llm::ChatCompletionResponse ok;
  ok.status = Status::Ok();
  ok.finish_reason = "stop";
  ok.model = "gpt-4o-mini";
  ok.content = VariantLlmJson();
  EXPECT_CALL(*mock, Chat(HasSubstr("RAG query-expansion expert"), _))
      .Times(AtLeast(1))
      .WillRepeatedly(Return(ok));
  return mock;
}

// Base fixture: real ingest assembly; derived tests seed two queryable documents in
// `kNs` after starting the server. The cross-NS query wiring is mounted by each
// derived test (with vs without an LLM).
class QueryLlmModesBase : public ::testing::Test {
 protected:
  void SetUp() override {
    cortrix::query::RagFusionMetrics::Instance().ResetForTest();
    h_ = std::make_unique<cortrix::test::FullStackE2E>();
    h_->BuildIngest(/*embedding_dim=*/128);
    perm_svc_ = std::make_unique<cortrix::tenant::PermissionService>(h_->global_db());
    // The cross-NS query authorizes a namespace when its owner tenant ==
    // the caller's tenant. We query with user_key (tenant "alice"), so own the NS by
    // "alice" — otherwise AuthorizeNamespaces returns CX_ERR_NS_UNAUTHORIZED.
    ASSERT_TRUE(h_->CreateNamespaceOwnedBy(kNs, "alice").ok());
    RegisterIngestRoutes();
  }

  // Mount the document routes just long enough to ingest (the query wiring is mounted
  // by the derived test before Start(); document routes can share the server).
  void RegisterIngestRoutes() {
    // Mount the document routes just long enough to ingest (the query wiring is mounted
    // by the derived test before Start(); document routes can share the server).
    cortrix::RegisterDocumentRoutes(h_->server(), h_->upload_handler(), h_->pool(),
                                    h_->auth());
  }

  // Drive one upload + status poll AFTER the server is started.
  std::string IngestDoc(const std::string& filename, const std::string& content,
                        const std::string& metadata_json) {
    auto c = h_->Client();
    httplib::MultipartFormDataItems items = {
        {"file", content, filename, "text/plain"},
        {"metadata", metadata_json, "metadata.json", "application/json"},
    };
    auto up = c.Post(std::string("/api/v1/namespaces/") + kNs + "/documents",
                     h_->Bearer(h_->user_key()), items);
    EXPECT_TRUE(up);
    if (!up) return "";
    EXPECT_EQ(up->status, 201) << up->body;
    if (up->status != 201) return "";
    const std::string doc_id = json::parse(up->body)["doc_id"].get<std::string>();
    // poll to "ready"
    for (int i = 0; i < 100; ++i) {
      auto st = c.Get(std::string("/api/v1/namespaces/") + kNs + "/documents/" + doc_id +
                          "/status",
                      h_->Bearer(h_->user_key()));
      if (st && st->status == 200 &&
          json::parse(st->body)["status"].get<std::string>() == "ready") {
        return doc_id;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ADD_FAILURE() << filename << " did not reach ready";
    return doc_id;
  }

  // Seed both docs after the HTTP server is running.
  void IngestSeedDocs() {
    original_doc_id_ = IngestDoc(
        "original_only.txt", OriginalOnlyDoc(),
        R"({"authors":["Baselinequarter"],"tags":["f36-original-only"]})");
    variant_doc_id_ = IngestDoc(
        "variant_only.txt", VariantOnlyDoc(),
        R"({"authors":["Zephyrmarker","Quasaranchor","Novatok"],"tags":["f36-variant-only"]})");
    ASSERT_FALSE(original_doc_id_.empty());
    ASSERT_FALSE(variant_doc_id_.empty());
  }

  // POST /api/v1/query forcing the complex route + rag-fusion opt-in (the two request
  // gates); whether the LLM is actually invoked then depends only on the wiring's llm.
  httplib::Result Query() {
    auto c = h_->Client();
    json body = {{"query", "baselinequarter"},
                 {"namespaces", json::array({kNs})},
                 {"route", "complex"},     // force routing_path == "complex"
                 {"rag_fusion", true},     // opt into RAG-Fusion
                 {"locale", "en"},         // BEIR-style English query expansion
                 {"granularity", "doc"},   // deterministic META block/doc summary metadata path
                 {"top_k", 2},
                 {"explain", true}};
    return c.Post("/api/v1/query?granularity=doc&explain=true",
                  h_->Bearer(h_->user_key()), body.dump(),
                  "application/json");
  }

  std::unique_ptr<cortrix::test::FullStackE2E> h_;
  std::unique_ptr<cortrix::tenant::PermissionService> perm_svc_;
  std::string original_doc_id_;
  std::string variant_doc_id_;
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
    IngestSeedDocs();
  }
  std::shared_ptr<llm::MockLlmClient> llm_;
  std::unique_ptr<cortrix::query::CrossNsQueryWiring> wiring_;
};

// The LLM-configured query runs rag-fusion: the MockLlmClient's Chat MUST be invoked
// (variant expansion entered the query chain) — the core "LLM full-chain" probe — and
// the fused query returns a 200 with a well-formed result envelope. RAG-Fusion uses
// anchored conservative weighted RRF, so the original strong hit should stay first
// while the variant-only doc still enters the final order as additive evidence.
TEST_F(QueryWithLlm, RagFusionInvokesLlmAndReturnsResults) {
  auto res = Query();
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  EXPECT_TRUE(j.contains("results"));
  EXPECT_TRUE(j["results"].is_array());
  ASSERT_FALSE(j["results"].empty()) << res->body;
  EXPECT_TRUE(HasResultDocId(j, variant_doc_id_)) << res->body;
  EXPECT_TRUE(HasResultDocId(j, original_doc_id_)) << res->body;
  EXPECT_LT(ResultDocPosition(j, original_doc_id_),
            ResultDocPosition(j, variant_doc_id_)) << res->body;
  EXPECT_TRUE(j.contains("meta"));
  ASSERT_TRUE(j.contains("explain"));
  ASSERT_TRUE(j["explain"].contains("llm_dependent_features"));
  const auto& features = j["explain"]["llm_dependent_features"];
  ASSERT_TRUE(features.contains("rag_fusion"));
  EXPECT_TRUE(features["rag_fusion"].value("active", false));
  EXPECT_EQ(features["rag_fusion"].value("variant_count", 0), 4);
  // Corroboration: rag-fusion recorded a non-disabled invocation (LLM path taken).
  // The hard proof is the EXPECT_CALL(Chat) on llm_, verified at fixture teardown.
}

// ── no LLM: query degrades gracefully (no RAG-Fusion, still usable) ───────────────
class QueryWithoutLlm : public QueryLlmModesBase {
 protected:
  void SetUp() override {
    QueryLlmModesBase::SetUp();
    wiring_ = std::make_unique<cortrix::query::CrossNsQueryWiring>(
        h_->pool(), h_->embedder(), h_->fusion(), *perm_svc_,
        /*sparse_registry=*/nullptr, /*llm=*/nullptr, /*engine_instr=*/nullptr);
    wiring_->Register(h_->server(), h_->auth());
    h_->Start();
    IngestSeedDocs();
  }
  std::unique_ptr<cortrix::query::CrossNsQueryWiring> wiring_;
};

// Even though the request opts into rag-fusion + the complex route, with no LLM wired
// RAG-Fusion cannot expand — the query must NOT crash and must still return a well-formed 200
// result set (vector + BM25 + RRF via the degradation path).
TEST_F(QueryWithoutLlm, NoLlmDegradesGracefully) {
  auto res = Query();
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  EXPECT_TRUE(j.contains("results"));
  EXPECT_TRUE(j["results"].is_array());
  ASSERT_FALSE(j["results"].empty()) << res->body;
  EXPECT_TRUE(HasResultDocId(j, original_doc_id_)) << res->body;
  EXPECT_FALSE(HasResultDocId(j, variant_doc_id_)) << res->body;
  ASSERT_TRUE(j.contains("meta"));
  // coverage_ratio is an always-present A-class field; the NS was queried, so the
  // degraded query still reports coverage over the one namespace.
  EXPECT_TRUE(j["meta"].contains("coverage_ratio"));
}

// ── benchmark / PR #7 regression: metadata-only META block authors enter doc_fts5 and the
// production cross-NS query doc/both path can retrieve them. This is the benchmark
// failure mode exposed during regression review: the LLM/product-side document evidence existed in
// design, but doc-level candidates were not guaranteed to enter the query candidate
// path. The probe deliberately keeps "Lovelace" OUT of the document text/filename and
// puts it only in upload metadata.authors, so a hit proves META block -> doc_fts5 -> query.
constexpr const char* kF44Ns = "f44_doc_fallback";

std::string F44MetadataOnlyProbeDoc() {
  return "F44 benchmark path validation note. The document discusses an analytical "
         "engine, early computing machinery, and retrieval candidate ordering.\n\n"
         "The author name used by this regression is intentionally stored only in "
         "upload metadata, not in this document body.";
}

bool HasResultDocId(const json& body, const std::string& doc_id) {
  if (!body.contains("results") || !body["results"].is_array()) return false;
  for (const auto& r : body["results"]) {
    if (r.value("doc_id", "") == doc_id) return true;
    if (r.value("child_id", "") == doc_id) return true;
    if (r.contains("metadata") && r["metadata"].is_object()) {
      const auto& m = r["metadata"];
      if (m.value("doc_id", "") == doc_id) return true;
      if (m.value("source_doc_id", "") == doc_id) return true;
      if (m.value("hybrid_doc_id", "") == doc_id) return true;
    }
  }
  return false;
}

int ResultDocPosition(const json& body, const std::string& doc_id) {
  if (!body.contains("results") || !body["results"].is_array()) return 1'000'000;
  for (size_t i = 0; i < body["results"].size(); ++i) {
    const auto& r = body["results"][i];
    if (r.value("doc_id", "") == doc_id) return static_cast<int>(i);
    if (r.value("child_id", "") == doc_id) return static_cast<int>(i);
    if (r.contains("metadata") && r["metadata"].is_object()) {
      const auto& m = r["metadata"];
      if (m.value("doc_id", "") == doc_id) return static_cast<int>(i);
      if (m.value("source_doc_id", "") == doc_id) return static_cast<int>(i);
      if (m.value("hybrid_doc_id", "") == doc_id) return static_cast<int>(i);
    }
  }
  return 1'000'000;
}

bool HasMetadataValue(const json& body, const std::string& key,
                      const std::string& value) {
  if (!body.contains("results") || !body["results"].is_array()) return false;
  for (const auto& r : body["results"]) {
    if (r.contains("metadata") && r["metadata"].is_object() &&
        r["metadata"].value(key, "") == value) {
      return true;
    }
  }
  return false;
}

bool HasMetadataKey(const json& body, const std::string& key) {
  if (!body.contains("results") || !body["results"].is_array()) return false;
  for (const auto& r : body["results"]) {
    if (r.contains("metadata") && r["metadata"].is_object() &&
        r["metadata"].contains(key)) {
      return true;
    }
  }
  return false;
}

class F44DocFtsFallbackE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    h_ = std::make_unique<cortrix::test::FullStackE2E>();
    h_->BuildIngest(/*embedding_dim=*/128);
    perm_svc_ = std::make_unique<cortrix::tenant::PermissionService>(h_->global_db());
    ASSERT_TRUE(h_->CreateNamespaceOwnedBy(kF44Ns, "alice").ok());

    cortrix::RegisterDocumentRoutes(h_->server(), h_->upload_handler(), h_->pool(),
                                    h_->auth());

    // Mount the same doc summary discover route bootstrap mounts, but over the harness parser
    // setup. The config leaves fts5_fallback_enabled=true (default).
    cortrix::doc_summary::DocSummaryConfig discover_cfg;
    h_->server().Get(
        "/api/v1/documents/discover",
        cortrix::WithAuth(
            h_->auth(), cortrix::kPermRead,
            [this, discover_cfg](const httplib::Request& req, httplib::Response& res,
                                 const cortrix::RequestContext& rctx) {
              cortrix::doc_summary::HandleDocumentsDiscover(
                  req, res, rctx, h_->pool(), h_->embedder(), discover_cfg);
            }));

    query_wiring_ = std::make_unique<cortrix::query::CrossNsQueryWiring>(
        h_->pool(), h_->embedder(), h_->fusion(), *perm_svc_,
        /*sparse_registry=*/nullptr, /*llm=*/nullptr, /*engine_instr=*/nullptr);
    query_wiring_->Register(h_->server(), h_->auth());

    h_->Start();
  }

  std::string IngestProbeDoc() {
    auto c = h_->Client();
    httplib::MultipartFormDataItems items = {
        {"file", F44MetadataOnlyProbeDoc(), "f44_metadata_probe.txt", "text/plain"},
        {"metadata", R"({"authors":["Ada","Lovelace"],"tags":["f44","metadata-only"]})",
         "metadata.json", "application/json"},
    };
    auto up = c.Post(std::string("/api/v1/namespaces/") + kF44Ns + "/documents",
                     h_->Bearer(h_->user_key()), items);
    EXPECT_TRUE(up);
    EXPECT_EQ(up ? up->status : 0, 201) << (up ? up->body : "no response");
    if (!up || up->status != 201) return "";
    const std::string doc_id = json::parse(up->body)["doc_id"].get<std::string>();
    for (int i = 0; i < 100; ++i) {
      auto st = c.Get(std::string("/api/v1/namespaces/") + kF44Ns + "/documents/" +
                          doc_id + "/status",
                      h_->Bearer(h_->user_key()));
      if (st && st->status == 200) {
        auto j = json::parse(st->body);
        const std::string status = j.value("status", "");
        if (status == "ready") return doc_id;
        if (status == "error") {
          ADD_FAILURE() << st->body;
          return "";
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ADD_FAILURE() << "F44 probe doc did not reach ready";
    return doc_id;
  }

  httplib::Result Query(const std::string& granularity,
                        const std::string& query = "Lovelace") {
    auto c = h_->Client();
    json body = {{"query", query},
                 {"namespaces", json::array({kF44Ns})},
                 {"route", "complex"},
                 {"granularity", granularity},
                 {"top_k", 5},
                 {"explain", true}};
    return c.Post("/api/v1/query?granularity=" + granularity + "&explain=true",
                  h_->Bearer(h_->user_key()), body.dump(), "application/json");
  }

  std::unique_ptr<cortrix::test::FullStackE2E> h_;
  std::unique_ptr<cortrix::tenant::PermissionService> perm_svc_;
  std::unique_ptr<cortrix::query::CrossNsQueryWiring> query_wiring_;
};

TEST_F(F44DocFtsFallbackE2E,
       InvalidCragQueryParamReturns400BeforeRetrieval) {
  auto c = h_->Client();
  json body = {{"query", "Lovelace"},
               {"namespaces", json::array({kF44Ns})},
               {"route", "complex"},
               {"granularity", "chunk"},
               {"top_k", 5}};
  auto res = c.Post("/api/v1/query?crag=maybe", h_->Bearer(h_->user_key()),
                    body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400) << res->body;
  EXPECT_THAT(res->body, HasSubstr("crag must be one of"));
}

TEST_F(F44DocFtsFallbackE2E,
       MetadataAuthorsReachDiscoverAndQueryDocAndBothGranularity) {
  const std::string doc_id = IngestProbeDoc();
  ASSERT_FALSE(doc_id.empty());

  auto c = h_->Client();
  auto discover = c.Get(std::string("/api/v1/documents/discover?namespace=") + kF44Ns +
                            "&query=Lovelace&top_k=5&explain=true",
                        h_->Bearer(h_->user_key()));
  ASSERT_TRUE(discover);
  ASSERT_EQ(discover->status, 200) << discover->body;
  const json d = json::parse(discover->body);
  ASSERT_TRUE(HasResultDocId(d, doc_id)) << discover->body;
  ASSERT_FALSE(d["results"].empty()) << discover->body;
  EXPECT_EQ(d["results"][0].value("via_path", ""), "fts5_fallback")
      << discover->body;

  auto qdoc_res = Query("doc");
  ASSERT_TRUE(qdoc_res);
  ASSERT_EQ(qdoc_res->status, 200) << qdoc_res->body;
  const json qdoc = json::parse(qdoc_res->body);
  EXPECT_TRUE(HasResultDocId(qdoc, doc_id)) << qdoc_res->body;
  EXPECT_TRUE(HasMetadataValue(qdoc, "via_path", "fts5_fallback"))
      << qdoc_res->body;
  EXPECT_TRUE(HasMetadataKey(qdoc, "doc_fts5_match_score")) << qdoc_res->body;
  ASSERT_TRUE(qdoc.contains("meta"));
  EXPECT_THAT(qdoc["meta"]["namespaces_succeeded"].get<std::vector<std::string>>(),
              ::testing::Contains(kF44Ns));
  ASSERT_TRUE(qdoc.contains("explain"));
  EXPECT_EQ(qdoc["explain"].value("granularity", ""), "doc");

  auto qboth_res = Query("both");
  ASSERT_TRUE(qboth_res);
  ASSERT_EQ(qboth_res->status, 200) << qboth_res->body;
  const json qboth = json::parse(qboth_res->body);
  EXPECT_TRUE(HasResultDocId(qboth, doc_id)) << qboth_res->body;
  EXPECT_TRUE(HasMetadataValue(qboth, "hybrid_has_doc", "true"))
      << qboth_res->body;
  EXPECT_TRUE(HasMetadataKey(qboth, "doc_fts5_match_score"))
      << qboth_res->body;
  ASSERT_TRUE(qboth.contains("explain"));
  EXPECT_EQ(qboth["explain"].value("granularity", ""), "both");

  auto qauto_res = Query("auto");
  ASSERT_TRUE(qauto_res);
  ASSERT_EQ(qauto_res->status, 200) << qauto_res->body;
  const json qauto = json::parse(qauto_res->body);
  EXPECT_TRUE(HasResultDocId(qauto, doc_id)) << qauto_res->body;
  EXPECT_TRUE(HasMetadataValue(qauto, "hybrid_has_doc", "true"))
      << qauto_res->body;
  ASSERT_TRUE(qauto.contains("explain"));
  EXPECT_EQ(qauto["explain"].value("granularity", ""), "auto");
}

}  // namespace
}  // namespace cortrix
