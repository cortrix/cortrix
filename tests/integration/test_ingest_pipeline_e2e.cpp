/// @file test_ingest_pipeline_e2e.cpp
/// @brief R7 full-stack E2E for the ingest chain over the PRODUCTION assembly
///        (real PhnswIndexFactory pool + real SPCManager + real SPCPipeline), driven
///        through the HTTP upload/status/query routes.
///
/// Why this exists (full-stack, catching live-only assembly
/// seams): the existing full-stack server tests (test_e2e_full_server /
/// test_cross_feature_e2e) back the server with a no-op TestSPCManager + a
/// non-searchable FakeIndex, so an uploaded document never actually flows through the
/// real SPC pipeline. The pipeline's optional seams — the enricher/contextual retrieval/HyPE EnricherChain,
/// the SparseIndexRegistry, the CleaningConfig resolver — are all installed
/// via Set* PROXIES *after* the pipeline has moved into the SPCManager. That
/// assembly-order wiring is exercised here for the first time end-to-end: a real file
/// is POSTed, the real SPCManager worker processes it, and the per-NS store is then
/// inspected to prove each seam's product actually landed.
///
/// Decision A (lead, R7): this test targets the WIRING, not semantic recall quality.
/// The embedder is the deterministic 128-dim stub, so the query assertion is the weak
/// "query path runs + returns a well-formed result set"; the STRONG probes are the
/// per-NS store reads (dedup collapse / sparse_vec / hype blocks / enrichment / per-NS
/// cleaning override). Real bge-m3 semantic recall is E2E-4.
///
/// If the hype/enrichment probes (D/E) come back empty, that is NOT a flaky test — it
/// means the three Set* seams did not take effect through the manager proxy, i.e. the
/// exact live-only assembly gap this suite hunts. Report it; do not weaken the probe.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "cortrix/llm/i_llm_client.h"                  // ILlmClient / ChatCompletionResponse
#include "cortrix/spc/enricher_chain.h"
#include "cortrix/spc/hype_enricher.h"                 // HyPEEnricher / HyPEConfig
#include "cortrix/spc_enricher.h"                      // EnrichResult / Entity / ISpcEnricher
#include "cortrix/spc/parser.h"                        // ChunkContext / DocumentMetadata
#include "cortrix/spc/cleaning_types.h"                // CleaningConfig
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/routes/document_routes.h"     // RegisterDocumentRoutes
#include "cortrix/query/query_routes.h"                // RegisterQueryRoutes

#include "mock_llm_client.h"                           // llm::MockLlmClient (tests/mocks)
#include "full_stack_e2e_harness.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using ::testing::_;
using ::testing::Return;

constexpr const char* kNsDedupOn = "sales";   // default cleaning (dedup on)
constexpr const char* kNsDedupOff = "eng";    // NS override: dedup off

// ── fake enrichers (mirror tests/unit/test_spc_pipeline_r7.cpp) ──────────────────

// enricher-style: stamps a summary + one entity + a positive enriched_score so the
// WriteEnrichment branch + entities table write run.
class FakeSummaryEnricher : public cortrix::spc::ISpcEnricher {
 public:
  cortrix::spc::EnrichResult Enrich(const std::string&,
                                    const cortrix::spc::DocumentMetadata&,
                                    const cortrix::spc::ChunkContext&) override {
    cortrix::spc::EnrichResult r;
    r.summary = "e2e fake summary";
    r.enricher_name = "LlmEnricher";
    r.enriched_score = 0.5f;
    cortrix::spc::Entity e;
    e.text = "Cortrix";
    e.type = "PRODUCT";
    r.entities.push_back(e);
    return r;
  }
  std::vector<cortrix::spc::EnrichResult> EnrichBatch(
      const std::vector<cortrix::spc::ChunkContext>& c) override {
    std::vector<cortrix::spc::EnrichResult> out;
    for (const auto& ctx : c) out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
    return out;
  }
  bool IsAvailable() const override { return true; }
  std::string Name() const override { return "LlmEnricher"; }
};

// contextual-style: stamps a contextualized embedding so the kFlagExtHasContextualized bit +
// WriteContextualized branch run.
class FakeContextualEnricher : public cortrix::spc::ISpcEnricher {
 public:
  cortrix::spc::EnrichResult Enrich(const std::string& text,
                                    const cortrix::spc::DocumentMetadata&,
                                    const cortrix::spc::ChunkContext&) override {
    cortrix::spc::EnrichResult r;
    r.contextualized_text = "ctx: " + text;
    r.contextualized_embedding = std::vector<float>(128, 0.01f);
    r.contextualized_status = 1;  // generated
    return r;
  }
  std::vector<cortrix::spc::EnrichResult> EnrichBatch(
      const std::vector<cortrix::spc::ChunkContext>& c) override {
    std::vector<cortrix::spc::EnrichResult> out;
    for (const auto& ctx : c) out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
    return out;
  }
  bool IsAvailable() const override { return true; }
  std::string Name() const override { return "ContextualRetrievalEnricher"; }
};

// HyPE LLM that returns exactly K=3 newline-separated questions for any prompt, so
// HyPEEnricher::GenerateHypeQuestions parses K questions (success path).
std::shared_ptr<llm::MockLlmClient> MakeHypeLlm() {
  auto mock = std::make_shared<llm::MockLlmClient>();
  llm::ChatCompletionResponse ok;
  ok.status = Status::Ok();
  ok.finish_reason = "stop";
  ok.model = "gpt-4o-mini";
  ok.content = "What is alpha?\nWhy beta?\nHow gamma?";  // exactly 3 lines (K default)
  EXPECT_CALL(*mock, Chat(_, _)).WillRepeatedly(Return(ok));
  return mock;
}

// ── document text: a long paragraph, verbatim-repeated ───────────────────────────
// The parent-child chunker accumulates paragraphs into parents (parent_size=1024 tok) then
// splits each parent into children of child_size=200 tok. To make duplication
// observable AT THE CHILD LEVEL, one paragraph must exceed child_size (so it splits
// into ≥2 children) and be repeated verbatim (so those children recur). dedup-on then
// collapses the recurring children; dedup-off keeps them — total > distinct.
//
// CRITICAL sizing (verified against chunker_config.h + BuildParentChild): paragraphs
// accumulate into a parent until parent_size=1024 tok, THEN flush; a unit that alone
// exceeds parent_size forms its OWN parent. So to make a verbatim-duplicate paragraph
// reproduce an IDENTICAL child sequence (which is what dedup acts on), each paragraph
// must exceed parent_size — then para1→parent1→[children] and the identical para2→
// parent2→[same children]. dedup-on collapses the recurring children across parents;
// dedup-off keeps them (total > distinct). (This is why test_spc_pipeline_r7 used a
// 4400-char paragraph; we use separator-rich sentences instead of a no-separator blob.)
std::string RepeatedParagraphDoc() {
  // ~160 short sentences ≈ 1200+ tokens > parent_size(1024) → its own parent, split
  // into several child_size=200 children.
  std::string para;
  for (int i = 0; i < 160; ++i) {
    para += "Sentence number " + std::to_string(i) +
            " describes how the semantic storage engine ingests and indexes content. ";
  }
  std::string doc;
  doc += para;  // paragraph 1 → its own parent (>1024 tok) → multi-child
  doc += "\n\n";
  doc += para;  // paragraph 2 — verbatim duplicate → its own parent → SAME children
  doc += "\n\n";
  doc += "A distinct closing paragraph about vector search and reranking that shares "
         "no sentences with the long paragraphs above.";
  return doc;
}

class IngestPipelineE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    h_ = std::make_unique<cortrix::test::FullStackE2E>();
    h_->BuildIngest(/*embedding_dim=*/128);

    // Install the enricher/contextual retrieval/HyPE chain on the shared chain object the harness already
    // handed to the manager (SetEnricherChain(&enricher_chain_) ran in BuildIngest;
    // it holds the POINTER, so appending here — before Start() / before any upload —
    // is the assembly-order seam under test).
    h_->enricher_chain().Append(std::make_shared<FakeSummaryEnricher>());
    h_->enricher_chain().Append(std::make_shared<FakeContextualEnricher>());
    h_->enricher_chain().Append(std::make_shared<cortrix::spc::HyPEEnricher>(
        cortrix::spc::HyPEConfig{}, MakeHypeLlm(), /*parent_store=*/nullptr));
    ASSERT_TRUE(h_->enricher_chain().AnyAvailable());

    // Cleaning: per-NS cleaning override via the manager proxy. "sales" keeps the default
    // (dedup on); "eng" disables dedup. Resolver keys off the namespace id.
    h_->spc_mgr().SetCleaningConfigResolver(
        [](const std::string& ns) -> cortrix::spc::CleaningConfig {
          cortrix::spc::CleaningConfig c;  // defaults: dedup_enabled = true
          if (ns == kNsDedupOff) c.dedup_enabled = false;
          return c;
        });

    // Admit both namespaces through the real catalog router, OWNED BY the tenant
    // of the API key these tests upload/query with. [V6] Namespace authorization is
    // now runtime PermissionService::BatchCheck (no static per-key allow-list): a
    // caller is authorized iff the namespace's owner_tenant_id == its
    // AuthContext.tenant_id. These tests drive the routes with h_->user_key(), whose
    // tenant_id is "alice" (MakeKey sets tenant_id == the key's user name). So the
    // namespaces MUST be owned by "alice" — CreateNamespace() owns by
    // "default_tenant", which alice is NOT authorized for (would 403 at upload).
    ASSERT_TRUE(h_->CreateNamespaceOwnedBy(kNsDedupOn, "alice").ok());
    ASSERT_TRUE(h_->CreateNamespaceOwnedBy(kNsDedupOff, "alice").ok());

    // Register the HTTP routes this E2E drives.
    cortrix::RegisterDocumentRoutes(h_->server(), h_->upload_handler(), h_->pool(),
                                    h_->auth());
    cortrix::RegisterQueryRoutes(h_->server(), h_->auth(), h_->pool(), h_->embedder(),
                                 h_->classifier(), h_->fusion());

    h_->Start();
  }

  // POST a document; returns its doc_id (asserts 201).
  std::string UploadDoc(const std::string& ns, const std::string& filename,
                        const std::string& content) {
    auto c = h_->Client();
    httplib::MultipartFormDataItems items = {
        {"file", content, filename, "text/plain"},
    };
    auto res = c.Post("/api/v1/namespaces/" + ns + "/documents",
                      h_->Bearer(h_->user_key()), items);
    EXPECT_TRUE(res);
    EXPECT_EQ(res->status, 201) << (res ? res->body : "no response");
    if (!res || res->status != 201) return "";
    return json::parse(res->body)["doc_id"].get<std::string>();
  }

  // Poll GET /status until the doc reaches a terminal state ("ready"/"error"); returns
  // the final status string ("" on timeout). The CE sync upload path enqueues to the
  // SPCManager worker, so processing is asynchronous — poll, never assume.
  std::string WaitForStatus(const std::string& ns, const std::string& doc_id,
                            int timeout_ms = 10000) {
    auto c = h_->Client();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string status;
    while (std::chrono::steady_clock::now() < deadline) {
      auto res = c.Get("/api/v1/namespaces/" + ns + "/documents/" + doc_id + "/status",
                       h_->Bearer(h_->user_key()));
      if (res && res->status == 200) {
        status = json::parse(res->body)["status"].get<std::string>();
        if (status == "ready" || status == "error") return status;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return status;  // last seen (or "") — caller asserts == "ready"
  }

  // Run a COUNT(*) query against the namespace's per-NS store db.
  int CountSql(const std::string& ns, const std::string& sql) {
    auto facade = h_->Facade(ns);
    EXPECT_TRUE(facade->Acquire().ok());
    sqlite3* db = facade->store().db_handle();
    EXPECT_NE(db, nullptr);
    sqlite3_stmt* st = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr), SQLITE_OK);
    int n = 0;
    if (st && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
  }

  // Surviving child texts for a doc (block_type=kBlockFile child rows).
  std::vector<std::string> ChildTexts(const std::string& ns, const std::string& doc_id) {
    auto facade = h_->Facade(ns);
    EXPECT_TRUE(facade->Acquire().ok());
    std::vector<cortrix::CortrixBlock> blocks;
    EXPECT_EQ(facade->store().block_get_by_doc(doc_id, blocks), 0);
    std::vector<std::string> out;
    for (const auto& b : blocks) {
      if (b.block_type == static_cast<int>(kBlockFile) && !b.child_id.empty())
        out.push_back(b.content_text);
    }
    return out;
  }

  std::unique_ptr<cortrix::test::FullStackE2E> h_;
};

// ── the full ingest chain end-to-end (sales NS, dedup on) ────────────────────────
// Upload → real SPCManager worker (parse → parent-child chunk → enricher/contextual retrieval/HyPE chain → clean
// → sparse → store/index) → poll ready → inspect per-NS store. Each assertion is
// a probe on one seam's product having actually landed through the manager proxy.
TEST_F(IngestPipelineE2E, FullChainProducesAllArtifacts) {
  const std::string doc_id = UploadDoc(kNsDedupOn, "doc.txt", RepeatedParagraphDoc());
  ASSERT_FALSE(doc_id.empty());
  ASSERT_EQ(WaitForStatus(kNsDedupOn, doc_id), "ready");

  // Semantic score/META block + parent-child chunking: child blocks were written.
  EXPECT_GT(CountSql(kNsDedupOn,
                     "SELECT COUNT(*) FROM blocks WHERE child_id IS NOT NULL "
                     "AND child_id != ''"),
            0)
      << "no child blocks — chunk/store chain did not run";

  // (E) enrichment landed: enriched_score + entities (WriteEnrichment branch).
  EXPECT_GT(CountSql(kNsDedupOn, "SELECT COUNT(*) FROM blocks WHERE enriched_score > 0"), 0)
      << "enricher enrichment missing — the EnricherChain seam did not take effect "
         "through the SPCManager proxy (live-only assembly gap — report, do not weaken)";
  EXPECT_GT(CountSql(kNsDedupOn, "SELECT COUNT(*) FROM entities"), 0)
      << "enricher entities missing — chain seam not wired through the manager";

  // (D) hype blocks landed (block_type=16=kBlockHypeQuestion). Their presence
  // proves the hype assembly + embed loop ran via the chain seam.
  EXPECT_GT(CountSql(kNsDedupOn, "SELECT COUNT(*) FROM blocks WHERE block_type = 16"), 0)
      << "hype blocks missing — chain seam (HyPEEnricher) did not run through "
         "the SPCManager proxy (live-only assembly gap — report, do not weaken)";

  // (C) sparse_vec persisted on at least one child (the SparseIndexRegistry seam).
  EXPECT_GT(CountSql(kNsDedupOn,
                     "SELECT COUNT(*) FROM blocks WHERE child_id IS NOT NULL "
                     "AND child_id != '' AND sparse_vec IS NOT NULL"),
            0)
      << "sparse retrieval sparse_vec missing — the SparseIndexRegistry seam did not take effect "
         "through the SPCManager proxy (live-only assembly gap — report, do not weaken)";
}

// (B/F) per-NS cleaning override: the SAME repeated-paragraph doc collapses its
// duplicate children in the dedup-on NS but keeps them in the dedup-off NS. This both
// proves dedup truly removes data AND that the cleaning resolver routed a different config
// per namespace through the manager proxy.
TEST_F(IngestPipelineE2E, PerNamespaceCleaningOverrideTakesEffect) {
  const std::string on_doc = UploadDoc(kNsDedupOn, "dup.txt", RepeatedParagraphDoc());
  ASSERT_FALSE(on_doc.empty());
  ASSERT_EQ(WaitForStatus(kNsDedupOn, on_doc), "ready");

  const std::string off_doc = UploadDoc(kNsDedupOff, "dup.txt", RepeatedParagraphDoc());
  ASSERT_FALSE(off_doc.empty());
  ASSERT_EQ(WaitForStatus(kNsDedupOff, off_doc), "ready");

  auto on_children = ChildTexts(kNsDedupOn, on_doc);
  auto off_children = ChildTexts(kNsDedupOff, off_doc);
  ASSERT_GT(on_children.size(), 0u);
  ASSERT_GT(off_children.size(), 0u);

  std::unordered_set<std::string> on_distinct(on_children.begin(), on_children.end());
  std::unordered_set<std::string> off_distinct(off_children.begin(), off_children.end());

  // dedup ON: no two surviving children share text (duplicates collapsed).
  EXPECT_EQ(on_children.size(), on_distinct.size())
      << "dedup-on NS kept duplicate children (cleaning seam not applied)";
  // dedup OFF: the verbatim-duplicate paragraph's children survive → total > distinct.
  EXPECT_GT(off_children.size(), off_distinct.size())
      << "dedup-off NS collapsed duplicates — the per-NS cleaning override did not "
         "route dedup_enabled=false through the SPCManager proxy";
}

// (A, decision A) The query path runs end-to-end over the real pool + index and
// returns a well-formed result envelope. Semantic recall *quality* (does the right
// chunk rank first) is E2E-4 (real bge-m3); here the stub embedder only guarantees
// the path executes and the response schema is intact.
TEST_F(IngestPipelineE2E, QueryPathRunsEndToEnd) {
  const std::string doc_id = UploadDoc(kNsDedupOn, "q.txt", RepeatedParagraphDoc());
  ASSERT_FALSE(doc_id.empty());
  ASSERT_EQ(WaitForStatus(kNsDedupOn, doc_id), "ready");

  auto c = h_->Client();
  json body = {{"query", "semantic storage engine"}, {"namespace", kNsDedupOn}, {"top_k", 5}};
  auto res = c.Post("/api/v1/query", h_->Bearer(h_->user_key()), body.dump(),
                    "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  ASSERT_TRUE(j.contains("results"));
  EXPECT_TRUE(j["results"].is_array());
  ASSERT_TRUE(j.contains("meta"));
  // The document's content is genuinely retrievable: BM25/FTS is exact text (does not
  // depend on the stub embedder's non-semantic vectors), so at least one result must
  // map back to the ingested doc.
  bool hit_uploaded = false;
  for (const auto& r : j["results"]) {
    if (r.contains("doc_id") && r["doc_id"].get<std::string>() == doc_id) hit_uploaded = true;
  }
  EXPECT_TRUE(hit_uploaded)
      << "ingested doc not retrievable via the query path (results=" << j["results"].dump()
      << ")";
}

}  // namespace
}  // namespace cortrix
