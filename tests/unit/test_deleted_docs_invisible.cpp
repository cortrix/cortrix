// #169: a soft-deleted document is invisible on every retrieval route and on the
// document discovery endpoint, as it already is to the list and status endpoints,
// and it comes back once restored. #157: so is a version of an uploaded or watched
// file that another version of it should answer for.
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
#include "cortrix/doc_summary/f41_schema_provider.h"    // kDocFts5IndexDdl
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

// A new version of an uploaded file is not served while the version it replaces is
// ready, even though its chunks are stored; once it is published, it is served and
// the retired one is not.
TEST_F(DeletedDocsInvisibleTest, UnpublishedReplacementIsHiddenBesideTheReadyVersion) {
    auto version = [&](const std::string& doc_id, DocStatus status, const char* created_at) {
        harness_->WithStore(kNs, [&](CortrixStore& store) {
            CortrixDoc doc;
            doc.doc_id = doc_id;
            doc.source_type = "http_upload";
            doc.source_path = "a.md";
            doc.status = status;
            ASSERT_EQ(store.doc_create(doc), 0);
            const std::string sql = std::string("UPDATE documents SET created_at = '") +
                                    created_at + "' WHERE doc_id = '" + doc_id + "'";
            ASSERT_EQ(sqlite3_exec(store.db_handle(), sql.c_str(), nullptr, nullptr, nullptr),
                      SQLITE_OK);
        });
    };
    version("doc-v1", DocStatus::kReady, "2026-10-01T00:00:00.000Z");
    version("doc-v2", DocStatus::kProcessing, "2026-10-02T00:00:00.000Z");
    const uint64_t v1 = SeedChunk("doc-v1", "c-v1", "match one");
    const uint64_t v2 = SeedChunk("doc-v2", "c-v2", "match two");
    harness_->fake_index()->set_search_result({{v1, 0.10f}, {v2, 0.20f}});
    const QueryContext ctx = ChunkCtx(/*vector=*/true, /*bm25=*/false, /*sparse=*/false);

    auto before = Ids(Run(ctx));
    EXPECT_TRUE(Has(before, "c-v1"));
    EXPECT_FALSE(Has(before, "c-v2")) << "the replacement is not served before it is ready";

    harness_->WithStore(kNs, [&](CortrixStore& store) {
        int64_t retired = 0;
        ASSERT_EQ(store.doc_publish_version("doc-v2", /*now_ms=*/1, &retired), 0);
        ASSERT_EQ(retired, 1);
    });
    auto after = Ids(Run(ctx));
    EXPECT_TRUE(Has(after, "c-v2"));
    EXPECT_FALSE(Has(after, "c-v1"));
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

TEST(LoadHiddenDocIdsTest, ReturnsOnlySoftDeletedRows) {
    CortrixStoreSqlite store(":memory:");
    ASSERT_EQ(store.Open(), 0);
    CortrixDoc live;
    live.source_type = "test";
    ASSERT_EQ(store.doc_create(live), 0);
    CortrixDoc gone;
    gone.source_type = "test";
    ASSERT_EQ(store.doc_create(gone), 0);
    ASSERT_EQ(store.doc_soft_delete(gone.doc_id, 1), 0);

    auto r = cortrix::store::LoadHiddenDocIds(
        store.db_handle(), {live.doc_id, gone.doc_id, gone.doc_id, "missing", ""});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), (std::unordered_set<std::string>{gone.doc_id}));
}

TEST(LoadHiddenDocIdsTest, BindsMoreIdsThanOneStatementHolds) {
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
    auto r = cortrix::store::LoadHiddenDocIds(store.db_handle(), ids);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), expected);
}

TEST(LoadHiddenDocIdsTest, NullHandleIsAnEmptySetAndAMissingTableAnError) {
    auto none = cortrix::store::LoadHiddenDocIds(nullptr, {"a"});
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().empty());

    sqlite3* bare = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &bare), SQLITE_OK);
    auto err = cortrix::store::LoadHiddenDocIds(bare, {"a"});
    EXPECT_FALSE(err.ok());
    sqlite3_close(bare);
}

// --- versions of one file: only the version that should answer is served ------------

class HiddenVersionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(store_.Open(), 0);
        Exec(cortrix::doc_summary::kDocFts5IndexDdl);
    }

    void Exec(const std::string& sql) {
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(store_.db_handle(), sql.c_str(), nullptr, nullptr, &err),
                  SQLITE_OK)
            << (err ? err : "") << " :: " << sql;
        sqlite3_free(err);
    }

    std::string Row(const std::string& type, const std::string& path, DocStatus status,
                    const std::string& created_at) {
        CortrixDoc d;
        d.source_type = type;
        d.source_path = path;
        d.status = status;
        EXPECT_EQ(store_.doc_create(d), 0);
        Exec("UPDATE documents SET created_at = '" + created_at + "' WHERE doc_id = '" +
             d.doc_id + "'");
        return d.doc_id;
    }

    // A chunk, and a document-level full-text row, that both match "quarterly".
    void Content(const std::string& doc_id, uint64_t block_id) {
        CortrixBlock b;
        b.block_id = block_id;
        b.doc_id = doc_id;
        b.child_id = "child-" + doc_id;
        b.block_type = static_cast<int>(kBlockFile);
        b.content_text = "quarterly report";
        b.metadata_json = "{}";
        b.data = {0x01};
        ASSERT_EQ(store_.block_insert(b), 0);
        Exec("INSERT INTO doc_fts5_index(doc_id, filename, doc_title, topics_rule_extracted,"
             " authors) VALUES ('" + doc_id + "', 'report.txt', 'quarterly report', '', '')");
    }

    std::unordered_set<std::string> Hidden(const std::vector<std::string>& ids) {
        auto r = cortrix::store::LoadHiddenDocIds(store_.db_handle(), ids);
        EXPECT_TRUE(r.ok()) << r.status().message();
        return r.ok() ? r.value() : std::unordered_set<std::string>{};
    }

    CortrixStoreSqlite store_{":memory:"};
};

constexpr const char* kD1 = "2026-10-01T00:00:00.000Z";
constexpr const char* kD2 = "2026-10-02T00:00:00.000Z";
constexpr const char* kD3 = "2026-10-03T00:00:00.000Z";
constexpr const char* kD4 = "2026-10-04T00:00:00.000Z";

TEST_F(HiddenVersionsTest, OnlyTheVersionThatShouldAnswerIsServed) {
    std::vector<std::string> all;
    auto row = [&](const std::string& type, const std::string& path, DocStatus status,
                   const char* created_at) {
        all.push_back(Row(type, path, status, created_at));
        return all.back();
    };
    // Replacements beside a ready earlier version, in each unfinished status.
    row("http_upload", "a.md", DocStatus::kReady, kD1);
    const auto pending = row("http_upload", "a.md", DocStatus::kPending, kD2);
    const auto processing = row("http_upload", "a.md", DocStatus::kProcessing, kD3);
    const auto error = row("http_upload", "a.md", DocStatus::kError, kD4);
    // An older unfinished row beside a newer ready one.
    const auto older = row("watch_dir", "/w/b.md", DocStatus::kProcessing, kD1);
    row("watch_dir", "/w/b.md", DocStatus::kReady, kD2);
    // No version ready: the oldest live one is served, a newer attempt is not.
    row("http_upload", "c.md", DocStatus::kError, kD1);
    const auto retry = row("http_upload", "c.md", DocStatus::kProcessing, kD2);
    // A first version, in any live status.
    for (DocStatus s : {DocStatus::kPending, DocStatus::kProcessing, DocStatus::kError,
                        DocStatus::kStale}) {
        row("http_upload", "first-" + std::to_string(static_cast<int>(s)) + ".md", s, kD1);
    }
    // Other types, the other versioned type at the same path, and another path.
    row("file", "e.md", DocStatus::kReady, kD1);
    row("file", "e.md", DocStatus::kProcessing, kD2);
    row("memory_session", "s-1", DocStatus::kReady, kD1);
    row("memory_session", "s-1", DocStatus::kPending, kD2);
    row("http_upload", "/w/f.md", DocStatus::kReady, kD1);
    row("watch_dir", "/w/f.md", DocStatus::kProcessing, kD2);
    row("http_upload", "g1.md", DocStatus::kReady, kD1);
    row("http_upload", "g2.md", DocStatus::kProcessing, kD2);
    // A legacy pair, both ready: both are served, as before.
    row("http_upload", "h.md", DocStatus::kReady, kD1);
    row("http_upload", "h.md", DocStatus::kReady, kD2);
    // Two attempts created in the same millisecond: the lower doc_id sorts first.
    const auto tie_a = row("http_upload", "t.md", DocStatus::kProcessing, kD1);
    const auto tie_b = row("http_upload", "t.md", DocStatus::kProcessing, kD1);
    // A deleted row.
    const auto deleted = row("file", "d.md", DocStatus::kReady, kD1);
    ASSERT_EQ(store_.doc_soft_delete(deleted, 1000), 0);

    const std::unordered_set<std::string> expected = {
        pending, processing, error, older, retry, std::max(tie_a, tie_b), deleted};
    EXPECT_EQ(Hidden(all), expected);
}

TEST_F(HiddenVersionsTest, FullTextRoutesServeOnlyTheVersionThatShouldAnswer) {
    const auto v1 = Row("http_upload", "a.md", DocStatus::kReady, kD1);
    const auto v2 = Row("http_upload", "a.md", DocStatus::kProcessing, kD2);
    const auto first = Row("http_upload", "z.md", DocStatus::kProcessing, kD1);
    Content(v1, 101);
    Content(v2, 102);
    Content(first, 103);

    std::vector<SearchResult> hits;
    ASSERT_EQ(store_.search_fulltext("quarterly", 10, hits), 0);
    std::unordered_set<std::string> bm25;
    for (const auto& h : hits) bm25.insert(h.doc_id);
    EXPECT_EQ(bm25, (std::unordered_set<std::string>{v1, first}));

    auto live = cortrix::doc_summary::SearchDocFts5(store_.db_handle(), "quarterly", 10,
                                                    /*live_documents_only=*/true);
    ASSERT_TRUE(live.ok()) << live.status().message();
    std::unordered_set<std::string> doc_level;
    for (const auto& h : live.value()) doc_level.insert(h.doc_id);
    EXPECT_EQ(doc_level, (std::unordered_set<std::string>{v1, first}));

    // The index-only query, which the standalone wrapper uses, is unchanged.
    auto all = cortrix::doc_summary::SearchDocFts5(store_.db_handle(), "quarterly", 10);
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all.value().size(), 3u);
}

// The condition keeps each statement on its indexes: the row's primary key or the
// full-text join outside, idx_doc_source inside. Unpinned, a lookup of more than a few
// ids turns into a scan of every versioned row. The statements are the production
// ones, captured while they run; the lookup carries 500 ids, as a full chunk does.
TEST_F(HiddenVersionsTest, QueryPlansStayOnTheirIndexes) {
    sqlite3* db = store_.db_handle();
    std::vector<std::string> captured;
    ASSERT_EQ(sqlite3_trace_v2(
                  db, SQLITE_TRACE_STMT,
                  [](unsigned, void* ctx, void* p, void*) -> int {
                      const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(p));
                      if (sql != nullptr) {
                          static_cast<std::vector<std::string>*>(ctx)->push_back(sql);
                      }
                      return 0;
                  },
                  &captured),
              SQLITE_OK);
    std::vector<std::string> ids;
    for (int i = 0; i < 500; ++i) ids.push_back("id-" + std::to_string(i));
    ASSERT_TRUE(cortrix::store::LoadHiddenDocIds(db, ids).ok());
    std::vector<SearchResult> hits;
    ASSERT_EQ(store_.search_fulltext("quarterly", 10, hits), 0);
    ASSERT_TRUE(cortrix::doc_summary::SearchDocFts5(db, "quarterly", 10, true).ok());
    sqlite3_trace_v2(db, 0, nullptr, nullptr);

    auto find = [&](const char* marker) {
        for (const auto& s : captured) {
            if (s.find(marker) != std::string::npos) return s;
        }
        ADD_FAILURE() << "statement not captured: " << marker;
        return std::string();
    };
    for (const std::string& sql : {find("FROM documents d WHERE d.doc_id IN"),
                                   find("FROM blocks_fts"),
                                   find("FROM doc_fts5_index JOIN documents")}) {
        if (sql.empty()) continue;
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(db, ("EXPLAIN QUERY PLAN " + sql).c_str(), -1, &st,
                                     nullptr),
                  SQLITE_OK)
            << sqlite3_errmsg(db);
        std::vector<std::string> plan;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* detail = sqlite3_column_text(st, 3);
            plan.push_back(detail ? reinterpret_cast<const char*>(detail) : "");
        }
        sqlite3_finalize(st);
        std::string text;
        for (const auto& line : plan) text += line + "\n";
        bool uses_source_index = false;
        for (const auto& line : plan) {
            if (line.find("idx_doc_source") != std::string::npos) uses_source_index = true;
            EXPECT_EQ(line.find("MULTI-INDEX OR"), std::string::npos) << text;
            if (line.rfind("SCAN ", 0) == 0) {
                EXPECT_NE(line.find("VIRTUAL TABLE"), std::string::npos)
                    << "only a full-text table may be scanned:\n" << text;
            }
        }
        EXPECT_TRUE(uses_source_index) << text;
    }
}

}  // namespace
}  // namespace cortrix::query
