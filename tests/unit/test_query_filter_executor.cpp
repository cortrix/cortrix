// The query filter (tags, date_range) on the live executor: a document that does not
// match is not served on the chunk path, the document path or both together, and the
// documents that do match fill top_k as though the others had never been retrieved.
//
// Deterministic, like test_deleted_docs_invisible.cpp: a real namespace pool and store
// over a temp dir, an injected index returning fixed hits, the stub embedder, an
// in-memory sparse registry, and rerank off. No model, no network, no clock.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "cortrix/common/block_types.h"
#include "cortrix/id/hash.h"
#include "cortrix/query/live_single_unit_executor.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/query_filter.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/retrieval/sparse_index_registry.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store_sqlite.h"
#include "fake_doc_discovery_deps.h"

namespace cortrix::query {
namespace {

using cortrix::doc_summary::test::DiscoveryPoolHarness;

constexpr const char* kNs = "query-filter-ns";

class QueryFilterExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                (std::string("cortrix_query_filter_") +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root_);
        harness_ = std::make_unique<DiscoveryPoolHarness>(root_);
        ASSERT_TRUE(harness_->Admit(kNs).ok());
        ASSERT_TRUE(embedder_.Init().ok());  // stub mode still needs Init
    }

    void TearDown() override {
        harness_.reset();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    // A chunk row as the executor resolves it: block_id is the child_id's hash.
    uint64_t SeedChunk(const std::string& doc_id, const std::string& child_id,
                       const std::string& text) {
        const uint64_t block_id = id::HashChildIdToBlockId(child_id);
        harness_->WithStore(kNs, [&](CortrixStore& store) {
            CortrixDoc doc;
            doc.doc_id = doc_id;
            doc.source_type = "test";
            store.doc_create(doc);  // a later chunk of the same document: harmless
            CortrixBlock b;
            b.block_id = block_id;
            b.doc_id = doc_id;
            b.child_id = child_id;
            b.block_type = static_cast<int>(kBlockFile);
            b.content_text = text;
            b.metadata_json = "{}";
            b.data = {0x01};  // blocks.data is NOT NULL
            ASSERT_EQ(store.block_insert(b), 0);
        });
        return block_id;
    }

    // Set what the filter reads: the document's upload metadata and its created_at
    // (stored as the server stores it, ISO 8601 UTC with milliseconds).
    void SetDoc(const std::string& doc_id, const std::string& metadata_json,
                const std::string& created_at = "2026-11-06T08:00:00.000Z") {
        harness_->WithStore(kNs, [&](CortrixStore& store) {
            sqlite3* db = store.db_handle();
            sqlite3_stmt* st = nullptr;
            ASSERT_EQ(sqlite3_prepare_v2(
                          db, "UPDATE documents SET metadata_json=?, created_at=? WHERE doc_id=?",
                          -1, &st, nullptr),
                      SQLITE_OK);
            sqlite3_bind_text(st, 1, metadata_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, created_at.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, doc_id.c_str(), -1, SQLITE_TRANSIENT);
            ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
            sqlite3_finalize(st);
            ASSERT_EQ(sqlite3_changes(db), 1) << doc_id;
        });
    }

    void AddSparse(const std::string& child_id, const std::string& text) {
        retrieval::ISparseRetriever* sp = sparse_.GetOrOpen(kNs);
        ASSERT_NE(sp, nullptr);
        EmbedWithSparseResult r;
        ASSERT_TRUE(embedder_.EmbedWithSparse(text, &r).ok());
        ASSERT_FALSE(r.sparse.empty());
        retrieval::SparseVector v;
        v.terms = r.sparse;
        ASSERT_TRUE(sp->Add(kNs, child_id, v).ok());
    }

    static QueryContext Ctx(const std::string& granularity, const nlohmann::json& filter,
                            int top_k = 5) {
        QueryContext ctx;
        ctx.query = "match";
        ctx.top_k = top_k;
        ctx.rerank = false;
        ctx.granularity = granularity;
        ctx.enable_vector = true;
        ctx.enable_bm25 = false;
        ctx.enable_sparse = false;
        EXPECT_TRUE(ParseQueryFilter(filter, &ctx.filter_spec).ok()) << filter.dump();
        return ctx;
    }

    retrieval::NamespaceQueryResult Run(const QueryContext& ctx) {
        LiveSingleUnitExecutor exec(harness_->ipool(), embedder_, fusion_,
                                    /*reranker=*/nullptr, &sparse_,
                                    /*candidate_multiplier=*/3, /*max_candidates=*/50,
                                    /*doc_fts5_fallback=*/false);
        return exec.ExecuteForNamespace(ctx, kNs);
    }

    static std::vector<std::string> Ids(const retrieval::NamespaceQueryResult& r) {
        std::vector<std::string> out;
        for (const auto& c : r.chunks) out.push_back(std::string(c.child_id));
        std::sort(out.begin(), out.end());
        return out;
    }

    // Three documents with one chunk each, all returned by the dense route.
    void SeedTaggedChunks() {
        const uint64_t c = SeedChunk("doc-contract", "c-contract", "match contract");
        const uint64_t i = SeedChunk("doc-invoice", "c-invoice", "match invoice");
        const uint64_t u = SeedChunk("doc-untagged", "c-untagged", "match untagged");
        SetDoc("doc-contract", R"({"tags":["contract","legal"]})");
        SetDoc("doc-invoice", R"({"tags":"invoice"})");  // a single string is a tag too
        SetDoc("doc-untagged", R"({"owner":"ops"})");
        harness_->fake_index()->set_search_result({{u, 0.10f}, {i, 0.20f}, {c, 0.30f}});
    }

    std::filesystem::path root_;
    std::unique_ptr<DiscoveryPoolHarness> harness_;
    OnnxEmbedder embedder_{/*model_path=*/""};  // empty path = stub mode
    RRFFusion fusion_;
    retrieval::SparseIndexRegistry sparse_{""};  // in-memory per-namespace indexes
};

using Ids_t = std::vector<std::string>;

// --- the chunk path ----------------------------------------------------------------

TEST_F(QueryFilterExecutorTest, ChunkPathWithoutFilterServesEveryDocument) {
    SeedTaggedChunks();
    EXPECT_EQ(Ids(Run(Ctx("chunk", nullptr))),
              (Ids_t{"c-contract", "c-invoice", "c-untagged"}));
}

TEST_F(QueryFilterExecutorTest, ChunkPathTagsServeOnlyMatchingDocuments) {
    SeedTaggedChunks();
    EXPECT_EQ(Ids(Run(Ctx("chunk", {{"tags", {"contract"}}}))), (Ids_t{"c-contract"}));
    EXPECT_EQ(Ids(Run(Ctx("chunk", {{"tags", {"invoice"}}}))), (Ids_t{"c-invoice"}));
    EXPECT_EQ(Ids(Run(Ctx("chunk", {{"tags", {"contract", "invoice"}}}))),
              (Ids_t{"c-contract", "c-invoice"}))
        << "any one of the requested tags is enough";
    EXPECT_TRUE(Ids(Run(Ctx("chunk", {{"tags", {"nothing"}}}))).empty());
}

TEST_F(QueryFilterExecutorTest, ChunkPathSparseRouteIsFilteredToo) {
    SeedChunk("doc-contract", "c-contract", "match contract");
    SeedChunk("doc-untagged", "c-untagged", "match untagged");
    SetDoc("doc-contract", R"({"tags":["contract"]})");
    SetDoc("doc-untagged", "{}");
    AddSparse("c-contract", "match contract");
    AddSparse("c-untagged", "match untagged");
    harness_->fake_index()->set_search_result({});
    QueryContext ctx = Ctx("chunk", {{"tags", {"contract"}}});
    ctx.enable_vector = false;
    ctx.enable_sparse = true;

    QueryContext control = ctx;
    control.filter_spec = QueryFilterSpec{};
    ASSERT_EQ(Ids(Run(control)), (Ids_t{"c-contract", "c-untagged"}))
        << "positive control: the sparse route serves both";
    EXPECT_EQ(Ids(Run(ctx)), (Ids_t{"c-contract"}));
}

TEST_F(QueryFilterExecutorTest, ChunkPathDateRangeIsInclusiveOnTheUtcDate) {
    const uint64_t a = SeedChunk("doc-a", "c-a", "match a");
    const uint64_t b = SeedChunk("doc-b", "c-b", "match b");
    const uint64_t c = SeedChunk("doc-c", "c-c", "match c");
    SetDoc("doc-a", "{}", "2026-11-04T23:59:59.999Z");
    SetDoc("doc-b", "{}", "2026-11-05T00:00:00.000Z");
    SetDoc("doc-c", "{}", "2026-11-06T23:59:59.999Z");
    harness_->fake_index()->set_search_result({{a, 0.10f}, {b, 0.20f}, {c, 0.30f}});

    auto range = [](const char* start, const char* end) {
        nlohmann::json r = nlohmann::json::object();
        if (start) r["start"] = start;
        if (end) r["end"] = end;
        return nlohmann::json{{"date_range", r}};
    };
    EXPECT_EQ(Ids(Run(Ctx("chunk", range("2026-11-05", "2026-11-06")))), (Ids_t{"c-b", "c-c"}));
    EXPECT_EQ(Ids(Run(Ctx("chunk", range("2026-11-05", "2026-11-05")))), (Ids_t{"c-b"}));
    EXPECT_EQ(Ids(Run(Ctx("chunk", range(nullptr, "2026-11-04")))), (Ids_t{"c-a"}));
    EXPECT_EQ(Ids(Run(Ctx("chunk", range("2026-11-06", nullptr)))), (Ids_t{"c-c"}));
    EXPECT_TRUE(Ids(Run(Ctx("chunk", range("2020-01-01", "2020-12-31")))).empty());
}

TEST_F(QueryFilterExecutorTest, ChunkPathTagsAndDateRangeMustBothHold) {
    const uint64_t old_c = SeedChunk("doc-old", "c-old", "match old");
    const uint64_t new_c = SeedChunk("doc-new", "c-new", "match new");
    SetDoc("doc-old", R"({"tags":["contract"]})", "2025-01-10T09:00:00.000Z");
    SetDoc("doc-new", R"({"tags":["contract"]})", "2026-11-05T09:00:00.000Z");
    harness_->fake_index()->set_search_result({{old_c, 0.10f}, {new_c, 0.20f}});
    EXPECT_EQ(Ids(Run(Ctx("chunk", {{"tags", {"contract"}},
                                    {"date_range", {{"start", "2026-01-01"}}}}))),
              (Ids_t{"c-new"}));
}

// Documents removed by the filter leave the lists before fusion, so matching documents
// ranked below them still fill top_k.
TEST_F(QueryFilterExecutorTest, ChunkPathMatchingDocumentsFillTopK) {
    std::vector<std::pair<uint64_t, float>> hits;
    for (int n = 0; n < 6; ++n) {
        const std::string d = "doc-" + std::to_string(n);
        hits.push_back({SeedChunk(d, "c-" + std::to_string(n), "match " + d), 0.1f * (n + 1)});
        SetDoc(d, n < 4 ? R"({"tags":["other"]})" : R"({"tags":["rare"]})");
    }
    harness_->fake_index()->set_search_result(hits);  // the four "other" rank first
    EXPECT_EQ(Ids(Run(Ctx("chunk", nullptr, /*top_k=*/2))), (Ids_t{"c-0", "c-1"}))
        << "positive control: without the filter the first two win";
    EXPECT_EQ(Ids(Run(Ctx("chunk", {{"tags", {"rare"}}}, /*top_k=*/2))),
              (Ids_t{"c-4", "c-5"}));
}

TEST_F(QueryFilterExecutorTest, KeysOutsideTheContractDoNotFilter) {
    SeedTaggedChunks();
    const QueryContext ctx = Ctx("chunk", {{"block_type", {"FILE"}}});
    EXPECT_FALSE(ctx.filter_spec.active());
    EXPECT_EQ(ctx.filter_spec.ignored_keys, (Ids_t{"block_type"}));
    EXPECT_EQ(Ids(Run(ctx)), (Ids_t{"c-contract", "c-invoice", "c-untagged"}));
}

// --- the document path and both paths together --------------------------------------

class QueryFilterDocumentPathTest : public QueryFilterExecutorTest {
protected:
    // Five documents, each with a summary (and, when `with_chunks`, one chunk); the
    // three untagged documents rank first, the two tagged ones last.
    void SeedDocuments(bool with_chunks) {
        std::vector<std::pair<uint64_t, float>> hits;
        for (int n = 0; n < 5; ++n) {
            const std::string d = "doc-" + std::to_string(n);
            const float score = 0.1f * (n + 1);
            hits.push_back({harness_->SeedDocSummaryBlock(kNs, d, "summary " + d, "{}"), score});
            if (with_chunks) {
                hits.push_back({SeedChunk(d, "c-" + std::to_string(n), "summary chunk " + d),
                                score});
            }
            SetDoc(d, n < 3 ? "{}" : R"({"tags":["contract"]})");
        }
        harness_->fake_index()->set_search_result(hits);
    }

    static QueryContext DocCtx(const std::string& granularity, const nlohmann::json& filter,
                               int top_k) {
        QueryContext ctx = Ctx(granularity, filter, top_k);
        ctx.query = "summary";
        return ctx;
    }
};

TEST_F(QueryFilterDocumentPathTest, DocumentPathServesOnlyMatchingDocuments) {
    SeedDocuments(/*with_chunks=*/false);
    ASSERT_EQ(Ids(Run(DocCtx("doc", nullptr, /*top_k=*/2))), (Ids_t{"doc-0", "doc-1"}))
        << "positive control: without the filter the untagged documents win";
    EXPECT_EQ(Ids(Run(DocCtx("doc", {{"tags", {"contract"}}}, /*top_k=*/2))),
              (Ids_t{"doc-3", "doc-4"}));
}

TEST_F(QueryFilterDocumentPathTest, DocumentPathStopsAtTopK) {
    SeedDocuments(/*with_chunks=*/false);
    EXPECT_EQ(Run(DocCtx("doc", {{"date_range", {{"start", "2026-01-01"}}}}, /*top_k=*/3))
                  .chunks.size(),
              3u);
}

TEST_F(QueryFilterDocumentPathTest, BothPathsTogetherServeOnlyMatchingDocuments) {
    SeedDocuments(/*with_chunks=*/true);
    const auto control = Run(DocCtx("both", nullptr, /*top_k=*/5));
    ASSERT_GE(control.chunks.size(), 3u) << "positive control: untagged documents are served";

    const auto result = Run(DocCtx("both", {{"tags", {"contract"}}}, /*top_k=*/5));
    ASSERT_FALSE(result.chunks.empty());
    for (const auto& c : result.chunks) {
        const std::string id(c.child_id);
        EXPECT_TRUE(id == "doc-3" || id == "doc-4" || id == "c-3" || id == "c-4") << id;
    }
}

}  // namespace
}  // namespace cortrix::query
