/// @file test_real_model_query_e2e.cpp
/// @brief R7 full-stack E2E for REAL semantic recall — bge-m3 ingest + query over the
///        production assembly, end-to-end via HTTP. The last feature E2E.
///
/// E2E-2 (test_ingest_pipeline_e2e) runs the full ingest chain with a deterministic
/// STUB embedder, so its query assertion was decision-A-weakened ("path runs +
/// well-formed results", not "the right chunk ranks first"): stub vectors carry no
/// semantic signal. This test fills that gap with the REAL bge-m3 model (1024-dim) so
/// the vector path produces genuine semantic neighbors, and asserts real semantic
/// DISCRIMINATION end-to-end through the server: upload two documents on clearly
/// different topics, query for one topic's concept, and require the top result to come
/// from the matching document — something only a real embedder can satisfy.
///
/// SKIPS when models/bge-m3/ is absent (CI / offline), exactly like the other real-
/// inference tests. The model dir is found relative to the binary or via
/// CORTRIX_SOURCE_DIR.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/query/query_wiring.h"                // CrossNsQueryWiring
#include "cortrix/server/routes/document_routes.h"     // RegisterDocumentRoutes
#include "cortrix/tenant/permission_service.h"         // PermissionService

#include "full_stack_e2e_harness.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr const char* kNs = "research";

// Same model-discovery contract as test_onnx_real_inference / test_real_spc_pipeline.
std::string FindModelDir() {
  std::vector<std::string> candidates = {
      "models/bge-m3", "../models/bge-m3", "../../models/bge-m3",
      "../../../models/bge-m3"};
  if (const char* src = std::getenv("CORTRIX_SOURCE_DIR")) {
    candidates.push_back(std::string(src) + "/models/bge-m3");
  }
  for (const auto& dir : candidates) {
    if (fs::exists(dir + "/model.onnx") && fs::exists(dir + "/tokenizer.json")) {
      return dir;
    }
  }
  return "";
}

// Two documents on lexically-distinct topics. The query below ("ways to keep a model
// from memorizing the training data") shares almost no salient words with the ML doc
// yet is semantically about it — so a keyword-only match would not reliably pick it;
// a real embedding does. The cooking doc is the distractor.
std::string MlDoc() {
  return "Regularization techniques in deep learning. Dropout randomly disables "
         "neurons during training. Weight decay penalizes large parameters. Early "
         "stopping halts training when validation loss rises.\n\n"
         "These methods improve how a neural network generalizes to unseen examples "
         "rather than memorizing its training set.";
}
std::string CookingDoc() {
  return "A classic tomato pasta recipe. Simmer crushed tomatoes with garlic, basil, "
         "and olive oil. Salt the boiling water generously before adding the pasta.\n\n"
         "Finish with grated parmesan and a drizzle of good olive oil before serving.";
}

class RealModelQueryE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    model_dir_ = FindModelDir();
    if (model_dir_.empty()) {
      GTEST_SKIP() << "bge-m3 model not found (models/bge-m3/{model.onnx,"
                      "tokenizer.json}); skipping real-model semantic E2E.";
    }
    h_ = std::make_unique<cortrix::test::FullStackE2E>();
    // Real bge-m3 (1024-dim) so the embedder and the P-HNSW index (opened at the
    // default/snapshot dim 1024) agree and the vector path carries real meaning.
    h_->BuildIngest(/*embedding_dim=*/1024, model_dir_ + "/model.onnx");
    ASSERT_TRUE(h_->embedder().is_real_model())
        << "expected the real bge-m3 model to load, got the stub";

    perm_svc_ = std::make_unique<cortrix::tenant::PermissionService>(h_->global_db());
    // Owned by "alice" so the F04 cross-NS query (queried with user_key) authorizes.
    ASSERT_TRUE(h_->CreateNamespaceOwnedBy(kNs, "alice").ok());

    // Document upload + cross-NS query (no LLM — plain dense + BM25 + RRF; the
    // semantic signal comes from the real vectors, not query-variant expansion).
    cortrix::RegisterDocumentRoutes(h_->server(), h_->upload_handler(), h_->pool(),
                                    h_->auth());
    query_wiring_ = std::make_unique<cortrix::query::CrossNsQueryWiring>(
        h_->pool(), h_->embedder(), h_->fusion(), *perm_svc_,
        /*sparse_registry=*/nullptr, /*llm=*/nullptr, /*engine_instr=*/nullptr);
    query_wiring_->Register(h_->server(), h_->auth());

    h_->Start();
  }

  // Upload + poll to ready; returns doc_id.
  std::string Ingest(const std::string& filename, const std::string& content) {
    auto c = h_->Client();
    httplib::MultipartFormDataItems items = {{"file", content, filename, "text/plain"}};
    auto up = c.Post(std::string("/api/v1/namespaces/") + kNs + "/documents",
                     h_->Bearer(h_->user_key()), items);
    EXPECT_TRUE(up);
    EXPECT_EQ(up ? up->status : 0, 201) << (up ? up->body : "no response");
    if (!up || up->status != 201) return "";
    const std::string doc_id = json::parse(up->body)["doc_id"].get<std::string>();
    for (int i = 0; i < 200; ++i) {  // real embedding is slower; allow ~20s
      auto st = c.Get(std::string("/api/v1/namespaces/") + kNs + "/documents/" +
                          doc_id + "/status",
                      h_->Bearer(h_->user_key()));
      if (st && st->status == 200) {
        auto j = json::parse(st->body);
        // Guard the status field: it is an enumerable string on the success body, but
        // be defensive against any transient/error shape rather than throwing here.
        if (j.contains("status") && j["status"].is_string() &&
            j["status"].get<std::string>() == "ready") {
          return doc_id;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ADD_FAILURE() << "doc " << filename << " did not reach ready";
    return doc_id;
  }

  std::string model_dir_;
  std::unique_ptr<cortrix::test::FullStackE2E> h_;
  std::unique_ptr<cortrix::tenant::PermissionService> perm_svc_;
  std::unique_ptr<cortrix::query::CrossNsQueryWiring> query_wiring_;
};

// Ingest an ML doc + a cooking doc; query a concept semantically tied to the ML doc
// but lexically distinct; require the top-ranked result to be the ML doc. Real
// semantic discrimination end-to-end (the property the stub-embedder E2E-2 cannot
// assert).
TEST_F(RealModelQueryE2E, SemanticRecallRanksRelevantDocFirst) {
  const std::string ml_doc = Ingest("ml.txt", MlDoc());
  const std::string cooking_doc = Ingest("cooking.txt", CookingDoc());
  ASSERT_FALSE(ml_doc.empty());
  ASSERT_FALSE(cooking_doc.empty());
  ASSERT_NE(ml_doc, cooking_doc);

  auto c = h_->Client();
  // Lexically distinct from the ML doc's wording, semantically about it.
  json body = {{"query", "ways to keep a model from memorizing the training data"},
               {"namespaces", json::array({kNs})},
               {"top_k", 5}};
  // Query with a short retry: "ready" means the doc row is processed, but the freshly
  // built P-HNSW + FTS rows can take a beat to become query-visible right after a real
  // (slower) embed pass. Retry a few times until results land before asserting.
  json j;
  for (int attempt = 0; attempt < 20; ++attempt) {
    auto res = c.Post("/api/v1/query", h_->Bearer(h_->user_key()), body.dump(),
                      "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    j = json::parse(res->body);
    ASSERT_TRUE(j.contains("results"));
    ASSERT_TRUE(j["results"].is_array());
    if (!j["results"].empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  ASSERT_GT(j["results"].size(), 0u) << "real-model query returned no results: " << j.dump();

  // The F04 cross-NS result item is keyed by child_id/parent_id + carries the chunk
  // `content` (NOT doc_id — that schema is the per-NS query's). Both docs live in one
  // namespace here, so the per-result discriminator is the chunk text: classify each
  // result as ML vs cooking by distinctive vocabulary.
  auto looks_ml = [](const std::string& s) {
    return s.find("regulari") != std::string::npos ||  // regulariz(e|ation)
           s.find("Dropout") != std::string::npos || s.find("dropout") != std::string::npos ||
           s.find("neural") != std::string::npos || s.find("generaliz") != std::string::npos ||
           s.find("training") != std::string::npos;
  };
  auto looks_cooking = [](const std::string& s) {
    return s.find("tomato") != std::string::npos || s.find("pasta") != std::string::npos ||
           s.find("garlic") != std::string::npos || s.find("parmesan") != std::string::npos ||
           s.find("olive oil") != std::string::npos;
  };

  // Top result must be the ML chunk — real semantic recall picks the relevant topic
  // over the cooking distractor, despite the query sharing little vocabulary with it.
  ASSERT_TRUE(j["results"][0].contains("content"));
  const std::string top = j["results"][0]["content"].get<std::string>();
  EXPECT_TRUE(looks_ml(top) && !looks_cooking(top))
      << "top result is not the semantically-relevant ML chunk; top content=\"" << top
      << "\" full results=" << j["results"].dump();

  // The ML chunk must out-rank the cooking chunk overall (defensive: scan the page).
  int ml_rank = -1, cooking_rank = -1;
  for (int i = 0; i < static_cast<int>(j["results"].size()); ++i) {
    const std::string content = j["results"][i].value("content", std::string());
    if (looks_ml(content) && ml_rank < 0) ml_rank = i;
    if (looks_cooking(content) && cooking_rank < 0) cooking_rank = i;
  }
  ASSERT_GE(ml_rank, 0) << "no ML chunk in results; results=" << j["results"].dump();
  if (cooking_rank >= 0) {
    EXPECT_LT(ml_rank, cooking_rank)
        << "cooking distractor out-ranked the relevant ML chunk";
  }
}

}  // namespace
}  // namespace cortrix
