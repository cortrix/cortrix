// #169: a soft-deleted document is invisible on every retrieval route and on the
// document discovery endpoint, as it already is to the list and status endpoints,
// and it comes back once restored.
//
// Deterministic: a real namespace pool and store over a temp dir, an injected index
// returning fixed hits, the stub embedder (its sparse half hashes each token, so a
// query and a chunk that share a word share a term), an in-memory sparse registry,
// and rerank off. No model, no network, no clock.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "cortrix/common/block_types.h"
#include "cortrix/doc_summary/discover_handler.h"
#include "cortrix/doc_summary/doc_fts5_index.h"
#include "cortrix/doc_summary/doc_summary_generator.h"  // DocSummaryConfig
#include "cortrix/id/hash.h"
#include "cortrix/query/live_single_unit_executor.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/retrieval/sparse_index_registry.h"
#include "cortrix/retrieval/sparse_rrf.h"
#include "cortrix/spc/contextual_store.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/doc_status_lookup.h"
#include "fake_doc_discovery_deps.h"

namespace cortrix::query {
namespace {

using cortrix::doc_summary::test::DiscoveryPoolHarness;

constexpr const char* kNs = "deleted-docs-ns";

class DeletedDocsInvisibleTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                (std::string("cortrix_deleted_docs_") +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root_);
        harness_ = std::make_unique<DiscoveryPoolHarness>(root_);
        ASSERT_TRUE(harness_->Admit(kNs).ok());
        // Stub mode still needs Init: until then every embed call fails, and the
        // vector, sparse and summary routes would yield nothing for a reason that
        // has nothing to do with deletion.
        ASSERT_TRUE(embedder_.Init().ok());
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

    // A hype question block that votes for `source_child_id`.
    void SeedHypeVote(const std::string& doc_id, const std::string& source_child_id,
                      uint64_t block_id) {
        harness_->WithStore(kNs, [&](CortrixStore& store) {
            CortrixBlock b;
            b.block_id = block_id;
            b.doc_id = doc_id;
            b.block_type = static_cast<int>(kBlockHypeQuestion);
            b.content_text = "a question about the chunk";
            b.metadata_json = nlohmann::json{{"source_child_id", source_child_id}}.dump();
            b.data = {0x01};
            ASSERT_EQ(store.block_insert(b), 0);
        });
    }

    // A contextual point's label row; the index then returns the label, which has
    // no blocks row of its own.
    uint64_t SeedContextualLabel(const std::string& child_id, uint64_t chunk_block_id) {
        const uint64_t label = cortrix::spc::DeriveContextualVecLabel(child_id);
        harness_->WithStore(kNs, [&](CortrixStore& store) {
            cortrix::spc::ContextualVecLabelRow row;
            row.label = label;
            row.block_id = chunk_block_id;
            row.child_id = child_id;
            ASSERT_TRUE(cortrix::spc::WriteContextualVecLabel(store.db_handle(), row).ok());
        });
        return label;
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

    void SoftDelete(const std::string& doc_id) {
        harness_->WithStore(kNs, [&](CortrixStore& store) {
            ASSERT_EQ(store.doc_soft_delete(doc_id, /*now_ms=*/1), 0);
        });
    }

    void Restore(const std::string& doc_id) {
        harness_->WithStore(kNs, [&](CortrixStore& store) {
            ASSERT_EQ(store.doc_restore(doc_id), 0);
        });
    }

    static QueryContext ChunkCtx(bool vector, bool bm25, bool sparse, int top_k = 5) {
        QueryContext ctx;
        ctx.query = "match";
        ctx.top_k = top_k;
        ctx.rerank = false;
        ctx.granularity = "chunk";
        ctx.enable_vector = vector;
        ctx.enable_bm25 = bm25;
        ctx.enable_sparse = sparse;
        return ctx;
    }

    retrieval::NamespaceQueryResult Run(const QueryContext& ctx,
                                        bool doc_fts5_fallback = false) {
        LiveSingleUnitExecutor exec(harness_->ipool(), embedder_, fusion_,
                                    /*reranker=*/nullptr, &sparse_,
                                    /*candidate_multiplier=*/3, /*max_candidates=*/50,
                                    doc_fts5_fallback);
        return exec.ExecuteForNamespace(ctx, kNs);
    }

    static std::vector<std::string> Ids(const retrieval::NamespaceQueryResult& r) {
        std::vector<std::string> out;
        for (const auto& c : r.chunks) out.push_back(std::string(c.child_id));
        return out;
    }

    static bool Has(const std::vector<std::string>& ids, const std::string& id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    std::filesystem::path root_;
    std::unique_ptr<DiscoveryPoolHarness> harness_;
    OnnxEmbedder embedder_{/*model_path=*/""};  // empty path = stub mode
    RRFFusion fusion_;
    retrieval::SparseIndexRegistry sparse_{""};  // in-memory per-namespace indexes
};

// --- the chunk path: every route that can surface a deleted document's chunk ------

TEST_F(DeletedDocsInvisibleTest, DenseHitIsDroppedAndComesBackOnRestore) {
    const uint64_t del = SeedChunk("doc-del", "c-del", "match deleted");
    const uint64_t live = SeedChunk("doc-live", "c-live", "match live");
    harness_->fake_index()->set_search_result({{del, 0.10f}, {live, 0.20f}});
    const QueryContext ctx = ChunkCtx(/*vector=*/true, /*bm25=*/false, /*sparse=*/false);

    auto before = Ids(Run(ctx));
    ASSERT_TRUE(Has(before, "c-del")) << "positive control: the dense hit is served";
    ASSERT_TRUE(Has(before, "c-live"));

    SoftDelete("doc-del");
    auto deleted = Ids(Run(ctx));
    EXPECT_FALSE(Has(deleted, "c-del"));
    EXPECT_TRUE(Has(deleted, "c-live"));

    Restore("doc-del");
    EXPECT_TRUE(Has(Ids(Run(ctx)), "c-del")) << "a restored document is served again";
}

TEST_F(DeletedDocsInvisibleTest, SparseHitIsDropped) {
    SeedChunk("doc-del", "c-del", "match deleted");
    SeedChunk("doc-live", "c-live", "match live");
    AddSparse("c-del", "match deleted");
    AddSparse("c-live", "match live");
    harness_->fake_index()->set_search_result({});
    const QueryContext ctx = ChunkCtx(/*vector=*/false, /*bm25=*/false, /*sparse=*/true);

    auto before = Ids(Run(ctx));
    ASSERT_TRUE(Has(before, "c-del")) << "positive control: the sparse hit is served";
    ASSERT_TRUE(Has(before, "c-live"));

    SoftDelete("doc-del");
    auto deleted = Ids(Run(ctx));
    EXPECT_FALSE(Has(deleted, "c-del"));
    EXPECT_TRUE(Has(deleted, "c-live"));
}

TEST_F(DeletedDocsInvisibleTest, HypeVoteIsDropped) {
    SeedChunk("doc-del", "c-del", "chunk text one");
    SeedChunk("doc-live", "c-live", "chunk text two");
    SeedHypeVote("doc-del", "c-del", /*block_id=*/9001);
    SeedHypeVote("doc-live", "c-live", /*block_id=*/9002);
    // The index returns only the question blocks, so the chunks arrive by vote alone.
    harness_->fake_index()->set_search_result({{9001, 0.10f}, {9002, 0.20f}});
    const QueryContext ctx = ChunkCtx(/*vector=*/true, /*bm25=*/false, /*sparse=*/false);

    auto before = Ids(Run(ctx));
    ASSERT_TRUE(Has(before, "c-del")) << "positive control: the hype vote is served";
    ASSERT_TRUE(Has(before, "c-live"));

    SoftDelete("doc-del");
    auto deleted = Ids(Run(ctx));
    EXPECT_FALSE(Has(deleted, "c-del"));
    EXPECT_TRUE(Has(deleted, "c-live"));
}

TEST_F(DeletedDocsInvisibleTest, ContextualVoteIsDropped) {
    const uint64_t del = SeedChunk("doc-del", "c-del", "chunk text one");
    const uint64_t live = SeedChunk("doc-live", "c-live", "chunk text two");
    const uint64_t del_label = SeedContextualLabel("c-del", del);
    const uint64_t live_label = SeedContextualLabel("c-live", live);
    // The index returns only the contextual points, so the chunks arrive by vote alone.
    harness_->fake_index()->set_search_result({{del_label, 0.10f}, {live_label, 0.20f}});
    const QueryContext ctx = ChunkCtx(/*vector=*/true, /*bm25=*/false, /*sparse=*/false);

    auto before = Ids(Run(ctx));
    ASSERT_TRUE(Has(before, "c-del")) << "positive control: the contextual vote is served";
    ASSERT_TRUE(Has(before, "c-live"));

    SoftDelete("doc-del");
    auto deleted = Ids(Run(ctx));
    EXPECT_FALSE(Has(deleted, "c-del"));
    EXPECT_TRUE(Has(deleted, "c-live"));
}

// The filter's one exemption: a hit resolved through the legacy reverse lookup
// carries no doc_id, so it cannot be matched to a document and is kept. A deleted
// document's ordinary hit rides along, so the filter is known to have run.
TEST_F(DeletedDocsInvisibleTest, HitWithoutADocIdIsKept) {
    SeedChunk("doc-del", "c-del", "match deleted");
    AddSparse("c-del", "match deleted");
    SoftDelete("doc-del");
    std::string legacy_child = "01LEGACYCHILD0000000000777";
    harness_->WithStore(kNs, [&](CortrixStore& store) {
        CortrixDoc doc;
        doc.doc_id = "doc-legacy";
        doc.source_type = "test";
        ASSERT_EQ(store.doc_create(doc), 0);
        CortrixBlock legacy;  // block_id left to the store: not the child_id's hash
        legacy.child_id = legacy_child;
        legacy.doc_id = "doc-legacy";
        legacy.block_type = static_cast<int>(kBlockFile);
        legacy.content_text = "match legacy";
        legacy.metadata_json = "{}";
        legacy.data = {0x01};
        ASSERT_EQ(store.block_insert(legacy), 0);
        ASSERT_NE(legacy.block_id, id::HashChildIdToBlockId(legacy_child));
    });
    AddSparse(legacy_child, "match legacy");
    harness_->fake_index()->set_search_result({});
    SoftDelete("doc-legacy");

    const auto ids = Ids(Run(ChunkCtx(/*vector=*/false, /*bm25=*/false, /*sparse=*/true)));
    EXPECT_FALSE(Has(ids, "c-del")) << "the filter ran and dropped the ordinary hit";
    EXPECT_TRUE(Has(ids, legacy_child));
}

// The deleted entries leave the lists before fusion, so the survivors' fused scores
// are exactly those of a fusion over the lists without them.
TEST_F(DeletedDocsInvisibleTest, SurvivorsAreRankedAsIfTheDeletedHitWasNeverRetrieved) {
    const uint64_t del = SeedChunk("doc-del", "c-del", "match deleted");
    const uint64_t a = SeedChunk("doc-a", "c-a", "match alpha");
    const uint64_t b = SeedChunk("doc-b", "c-b", "match bravo");
    harness_->fake_index()->set_search_result({{del, 0.10f}, {a, 0.20f}, {b, 0.30f}});
    SoftDelete("doc-del");

    const auto result = Run(ChunkCtx(/*vector=*/true, /*bm25=*/false, /*sparse=*/false));
    ASSERT_EQ(Ids(result), (std::vector<std::string>{"c-a", "c-b"}));

    retrieval::FivePathInput expected_in;
    retrieval::SparseHit ha;
    ha.child_id = "c-a";
    ha.score = 0.2f;
    retrieval::SparseHit hb;
    hb.child_id = "c-b";
    hb.score = 0.3f;
    expected_in.dense = {ha, hb};
    const auto expected = retrieval::FuseFivePathRrf(expected_in, /*top_n=*/0);
    ASSERT_EQ(expected.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(std::string(result.chunks[i].child_id), std::string(expected[i].child_id));
        EXPECT_FLOAT_EQ(result.chunks[i].score, expected[i].rrf_score);
    }
}

// --- the document path: the summary recall and its full-text fallback --------------

class DeletedDocsDocumentPathTest : public DeletedDocsInvisibleTest {
protected:
    // Three summaries: the deleted document's ranks first, two live ones after it.
    void SeedThreeSummaries() {
        const uint64_t del = harness_->SeedDocSummaryBlock(kNs, "doc-del", "summary deleted", "{}");
        const uint64_t l1 = harness_->SeedDocSummaryBlock(kNs, "doc-l1", "summary one", "{}");
        const uint64_t l2 = harness_->SeedDocSummaryBlock(kNs, "doc-l2", "summary two", "{}");
        harness_->fake_index()->set_search_result({{del, 0.10f}, {l1, 0.20f}, {l2, 0.30f}});
        SoftDelete("doc-del");
    }
};

TEST_F(DeletedDocsDocumentPathTest, ExecutorDocumentPathFillsTopKWithLiveDocuments) {
    SeedThreeSummaries();
    QueryContext ctx;
    ctx.query = "summary";
    ctx.top_k = 2;
    ctx.rerank = false;
    ctx.granularity = "doc";
    const auto result = Run(ctx, /*doc_fts5_fallback=*/false);
    EXPECT_EQ(Ids(result), (std::vector<std::string>{"doc-l1", "doc-l2"}));
}

TEST_F(DeletedDocsDocumentPathTest, DiscoverFillsTopKWithLiveDocuments) {
    SeedThreeSummaries();
    cortrix::doc_summary::DocSummaryConfig cfg;
    cfg.fts5_fallback_enabled = false;
    const nlohmann::json out = cortrix::doc_summary::ExecuteDocDiscovery(
        harness_->ipool(), embedder_, kNs, "summary", /*top_k=*/2, /*explain=*/false, cfg);
    std::vector<std::string> ids;
    for (const auto& r : out["results"]) ids.push_back(r.value("doc_id", ""));
    EXPECT_EQ(ids, (std::vector<std::string>{"doc-l1", "doc-l2"}));
}

TEST_F(DeletedDocsDocumentPathTest, FullTextFallbackHidesDeletedAndPurgedDocuments) {
    harness_->CreateFts5Table(kNs, "doc-live", "report.txt", "quarterly report", "", "");
    harness_->CreateFts5Table(kNs, "doc-del", "report.txt", "quarterly report draft", "", "");
    SoftDelete("doc-del");
    // Purge residue: a full-text row whose document row is gone.
    harness_->WithStore(kNs, [&](CortrixStore& store) {
        const char* ins =
            "INSERT INTO doc_fts5_index(doc_id, filename, doc_title, topics_rule_extracted,"
            " authors) VALUES ('doc-purged', 'report.txt', 'quarterly report old', '', '')";
        ASSERT_EQ(sqlite3_exec(store.db_handle(), ins, nullptr, nullptr, nullptr), SQLITE_OK);
    });
    harness_->fake_index()->set_search_result({});  // no summaries: the fallback alone

    cortrix::resource::NamespaceFacade facade(harness_->ipool(), kNs);
    ASSERT_TRUE(facade.Acquire().ok());
    cortrix::doc_summary::DocDiscoveryCoreOptions opts;
    opts.top_k = 5;
    opts.fts5_fallback_enabled = true;
    const auto core = cortrix::doc_summary::RunDocDiscoveryCore(
        facade.vec_index(), facade.store(), embedder_, "report", opts);
    ASSERT_FALSE(core.fts5_failed) << "the joined query prepared and ran";
    std::vector<std::string> ids;
    for (const auto& h : core.hits) ids.push_back(h.doc_id);
    EXPECT_EQ(ids, (std::vector<std::string>{"doc-live"}));

    // The index-only query, which the standalone wrapper uses, is unchanged.
    auto all = cortrix::doc_summary::SearchDocFts5(facade.store().db_handle(), "report", 5);
    ASSERT_TRUE(all.ok());
    std::unordered_set<std::string> all_ids;
    for (const auto& h : all.value()) all_ids.insert(h.doc_id);
    EXPECT_EQ(all_ids, (std::unordered_set<std::string>{"doc-live", "doc-del", "doc-purged"}));
}

// --- the helper ---------------------------------------------------------------------

TEST(LoadDeletedDocIdsTest, ReturnsOnlySoftDeletedRows) {
    CortrixStoreSqlite store(":memory:");
    ASSERT_EQ(store.Open(), 0);
    CortrixDoc live;
    live.source_type = "test";
    ASSERT_EQ(store.doc_create(live), 0);
    CortrixDoc gone;
    gone.source_type = "test";
    ASSERT_EQ(store.doc_create(gone), 0);
    ASSERT_EQ(store.doc_soft_delete(gone.doc_id, 1), 0);

    auto r = cortrix::store::LoadDeletedDocIds(
        store.db_handle(), {live.doc_id, gone.doc_id, gone.doc_id, "missing", ""});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), (std::unordered_set<std::string>{gone.doc_id}));
}

TEST(LoadDeletedDocIdsTest, BindsMoreIdsThanOneStatementHolds) {
    CortrixStoreSqlite store(":memory:");
    ASSERT_EQ(store.Open(), 0);
    std::vector<std::string> ids;
    std::unordered_set<std::string> expected;
    for (int i = 0; i < 1200; ++i) {
        CortrixDoc d;
        d.source_type = "test";
        ASSERT_EQ(store.doc_create(d), 0);
        ids.push_back(d.doc_id);
        if (i % 100 == 99) {  // one deleted document in each run of 100, across chunks
            ASSERT_EQ(store.doc_soft_delete(d.doc_id, 1), 0);
            expected.insert(d.doc_id);
        }
    }
    auto r = cortrix::store::LoadDeletedDocIds(store.db_handle(), ids);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), expected);
}

TEST(LoadDeletedDocIdsTest, NullHandleIsAnEmptySetAndAMissingTableAnError) {
    auto none = cortrix::store::LoadDeletedDocIds(nullptr, {"a"});
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().empty());

    sqlite3* bare = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &bare), SQLITE_OK);
    auto err = cortrix::store::LoadDeletedDocIds(bare, {"a"});
    EXPECT_FALSE(err.ok());
    sqlite3_close(bare);
}

}  // namespace
}  // namespace cortrix::query
