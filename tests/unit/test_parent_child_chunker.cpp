#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "cortrix/chunker/chunker_errors.h"
#include "cortrix/chunker/parent_child_chunker.h"
#include "cortrix/spc/parser.h"

namespace cortrix::chunker {
namespace {

using cortrix::spc::ParsedChunk;
using cortrix::spc::ParsedPage;

// Build a ParsedPage with the given paragraph texts.
ParsedPage MakePage(int page_num, const std::vector<std::string>& paras) {
    ParsedPage p;
    p.page_num = page_num;
    for (const auto& t : paras) {
        ParsedChunk c;
        c.text = t;
        c.page = page_num;
        p.paragraphs.push_back(c);
        if (!p.page_text.empty()) p.page_text += "\n";
        p.page_text += t;
    }
    return p;
}

ChunkerInput MakeInput(const std::vector<ParsedPage>& pages,
                       const std::string& doc_id = "DOC01",
                       const std::string& ns = "NS01") {
    ChunkerInput in;
    in.doc_id = doc_id;
    in.namespace_id = ns;
    in.pages = pages;
    in.metadata.doc_title = "Test Doc";
    in.metadata.doc_language = "en";
    in.metadata.mime_type = "text/plain";
    return in;
}

// Repeat a word n times into a single paragraph (~n tokens under EstimateTokens).
std::string Words(int n, const std::string& w = "word") {
    std::string s;
    for (int i = 0; i < n; ++i) { if (i) s += " "; s += w; }
    return s;
}

// --- EstimateTokens heuristic ---------------------------------------------------

TEST(EstimateTokensTest, EmptyIsZero) {
    EXPECT_EQ(EstimateTokens(""), 0u);
}

TEST(EstimateTokensTest, AsciiWordsCounted) {
    EXPECT_GT(EstimateTokens("hello world foo bar"), 0u);
    // Whitespace-only is zero tokens.
    EXPECT_EQ(EstimateTokens("   \n\t "), 0u);
}

TEST(EstimateTokensTest, CjkApproxOnePerChar) {
    // 4 CJK chars -> ~4 tokens.
    EXPECT_EQ(EstimateTokens("\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95"), 4u);
}

// --- Core parent-child chunking -------------------------------------------------

TEST(ParentChildChunkerTest, EmptyDocumentReturnsError) {
    ParentChildChunker chunker;
    auto r = chunker.Chunk(MakeInput({}));
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find(chunk_errors::kEmptyDocument), std::string::npos);
}

TEST(ParentChildChunkerTest, AllEmptyParagraphsIsEmptyDocument) {
    auto r = ParentChildChunker().Chunk(MakeInput({MakePage(1, {"", "   ", "\n"})}));
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find(chunk_errors::kEmptyDocument), std::string::npos);
}

TEST(ParentChildChunkerTest, SmallDocSingleParent) {
    auto r = ParentChildChunker().Chunk(MakeInput({MakePage(1, {"Hello world. This is a test."})}));
    ASSERT_TRUE(r.ok()) << r.status().message();
    const auto& out = r.value();
    EXPECT_EQ(out.parents.size(), 1u);
    EXPECT_GE(out.children.size(), 1u);
    EXPECT_EQ(out.stats.total_parents, 1u);
    EXPECT_EQ(out.stats.total_children, out.children.size());
}

TEST(ParentChildChunkerTest, ChildrenLinkBackToParent) {
    ChunkerConfig cfg;
    cfg.parent_size = 256;
    cfg.child_size = 50;
    ParentChildChunker chunker(cfg);
    // Two large paragraphs -> at least one parent with multiple children.
    auto r = chunker.Chunk(MakeInput({MakePage(1, {Words(120), Words(120, "text")})}));
    ASSERT_TRUE(r.ok()) << r.status().message();
    const auto& out = r.value();
    ASSERT_FALSE(out.parents.empty());
    ASSERT_FALSE(out.children.empty());

    // Every child's parent_id is a real parent; every parent's child_ids exist.
    std::set<std::string> parent_ids;
    for (const auto& p : out.parents) parent_ids.insert(p.parent_id);
    for (const auto& c : out.children) {
        EXPECT_TRUE(parent_ids.count(c.parent_id)) << "orphan child " << c.child_id;
    }
    size_t total_child_ids = 0;
    std::set<std::string> child_ids;
    for (const auto& c : out.children) child_ids.insert(c.child_id);
    for (const auto& p : out.parents) {
        total_child_ids += p.child_ids.size();
        for (const auto& cid : p.child_ids)
            EXPECT_TRUE(child_ids.count(cid)) << "dangling child_id " << cid;
    }
    EXPECT_EQ(total_child_ids, out.children.size());
}

TEST(ParentChildChunkerTest, IdsAreUlidsAndUnique) {
    ParentChildChunker chunker;
    auto r = chunker.Chunk(MakeInput({MakePage(1, {Words(300)})}));
    ASSERT_TRUE(r.ok());
    std::set<std::string> ids;
    for (const auto& p : r.value().parents) {
        EXPECT_EQ(p.parent_id.size(), 26u);
        EXPECT_TRUE(ids.insert(p.parent_id).second);
    }
    for (const auto& c : r.value().children) {
        EXPECT_EQ(c.child_id.size(), 26u);
        EXPECT_TRUE(ids.insert(c.child_id).second);
    }
}

TEST(ParentChildChunkerTest, ChunkIndexIsSequential) {
    ChunkerConfig cfg;
    cfg.parent_size = 128;
    cfg.child_size = 40;
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePage(1, {Words(400)})}));
    ASSERT_TRUE(r.ok());
    const auto& children = r.value().children;
    ASSERT_GE(children.size(), 2u);
    for (size_t i = 0; i < children.size(); ++i) {
        EXPECT_EQ(children[i].chunk_index, static_cast<uint32_t>(i));
    }
}

TEST(ParentChildChunkerTest, MultipleParentsWhenExceedingParentSize) {
    ChunkerConfig cfg;
    cfg.parent_size = 100;
    cfg.child_size = 50;
    // Five ~80-token paragraphs -> several parents.
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePage(1, {
        Words(80, "alpha"), Words(80, "bravo"), Words(80, "charlie"),
        Words(80, "delta"), Words(80, "echo")})}));
    ASSERT_TRUE(r.ok());
    EXPECT_GE(r.value().parents.size(), 2u);
}

TEST(ParentChildChunkerTest, ParentTokenCountWithinBudget) {
    ChunkerConfig cfg;
    cfg.parent_size = 100;
    cfg.child_size = 30;
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePage(1, {
        Words(40, "a"), Words(40, "b"), Words(40, "c"), Words(40, "d")})}));
    ASSERT_TRUE(r.ok());
    // Each non-final parent should be roughly within budget; a parent formed from
    // a single oversized unit may exceed, but here units are 40 < 100.
    for (const auto& p : r.value().parents) {
        EXPECT_GT(p.token_count, 0u);
        EXPECT_LE(p.token_count, static_cast<uint32_t>(cfg.parent_size) + 40u);
    }
}

// --- metadata inheritance (UT-F34-5) -------------------------------------------

TEST(ParentChildChunkerTest, MetadataInheritedThreeLayers) {
    ChunkerInput in = MakeInput({MakePage(1, {Words(300)})});
    in.metadata.doc_title = "Legal Contract";
    in.metadata.doc_language = "zh";
    in.metadata.mime_type = "application/pdf";

    auto r = ParentChildChunker().Chunk(in);
    ASSERT_TRUE(r.ok());
    ASSERT_FALSE(r.value().parents.empty());
    ASSERT_FALSE(r.value().children.empty());

    for (const auto& p : r.value().parents) {
        EXPECT_EQ(p.doc_id, "DOC01");
        EXPECT_EQ(p.namespace_id, "NS01");
        EXPECT_EQ(p.metadata.doc_language, "zh");
        EXPECT_EQ(p.metadata.mime_type, "application/pdf");
    }
    for (const auto& c : r.value().children) {
        EXPECT_EQ(c.doc_id, "DOC01");
        EXPECT_EQ(c.namespace_id, "NS01");
        EXPECT_EQ(c.metadata.doc_language, "zh");  // child inherits from doc/parent
        EXPECT_EQ(c.metadata.doc_title, "Legal Contract");
    }
}

// --- page provenance + per-page tolerance (UT-F34-4) ---------------------------

TEST(ParentChildChunkerTest, PageRangeTracked) {
    ChunkerConfig cfg;
    cfg.parent_size = 100000;  // force everything into one parent
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({
        MakePage(3, {"first page text here"}),
        MakePage(4, {"second page text here"}),
        MakePage(5, {"third page text here"})}));
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().parents.size(), 1u);
    EXPECT_EQ(r.value().parents[0].page_start, 3u);
    EXPECT_EQ(r.value().parents[0].page_end, 5u);
}

TEST(ParentChildChunkerTest, FailedPageSkippedAndCounted) {
    // UT-F34-4: a page F06 could not parse arrives empty (no paragraphs, empty
    // page_text); it is skipped and counted in stats.failed_pages.
    ParsedPage good = MakePage(1, {"usable content on page one"});
    ParsedPage bad;  // page 2: empty (simulated F06 failure)
    bad.page_num = 2;
    ParsedPage good2 = MakePage(3, {"usable content on page three"});

    auto r = ParentChildChunker().Chunk(MakeInput({good, bad, good2}));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().stats.total_pages, 3u);
    EXPECT_EQ(r.value().stats.succeeded_pages, 2u);
    EXPECT_EQ(r.value().stats.failed_pages, 1u);
    EXPECT_DOUBLE_EQ(r.value().stats.CoverageRatio(), 2.0 / 3.0);
}

// --- unit_level variants --------------------------------------------------------

TEST(ParentChildChunkerTest, PageUnitLevelUsesWholePageText) {
    ChunkerConfig cfg;
    cfg.unit_level = UnitLevel::kPage;
    cfg.parent_size = 100000;
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePage(1, {"para one", "para two"})}));
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().parents.size(), 1u);
    // Whole page_text (both paragraphs) is in the single parent.
    EXPECT_NE(r.value().parents[0].parent_text.find("para one"), std::string::npos);
    EXPECT_NE(r.value().parents[0].parent_text.find("para two"), std::string::npos);
}

TEST(ParentChildChunkerTest, SentenceUnitLevelSplitsPageText) {
    ChunkerConfig cfg;
    cfg.unit_level = UnitLevel::kSentence;
    cfg.parent_size = 100;
    cfg.child_size = 30;
    // Page with several sentences; sentence unit_level splits page_text into
    // sentence-sized units before parent accumulation.
    ParsedPage p;
    p.page_num = 1;
    p.page_text = "First sentence here. Second sentence here. Third one. Fourth follows.";
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({p}));
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_FALSE(r.value().parents.empty());
    EXPECT_FALSE(r.value().children.empty());
    EXPECT_EQ(r.value().stats.succeeded_pages, 1u);
}

TEST(ParentChildChunkerTest, OversizedSingleUnitFormsOwnParent) {
    // A single paragraph larger than parent_size must still form a parent (the
    // "take at least one unit even if it alone exceeds parent_size" branch), and
    // get split into multiple children.
    ChunkerConfig cfg;
    cfg.parent_size = 50;
    cfg.child_size = 20;
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePage(1, {Words(200)})}));
    ASSERT_TRUE(r.ok());
    EXPECT_GE(r.value().parents.size(), 1u);
    EXPECT_GE(r.value().children.size(), 2u);
}

TEST(ParentChildChunkerTest, ParagraphFallsBackToPageTextWhenNoParagraphs) {
    // A page with page_text but empty paragraphs[] still chunks (whole-page unit).
    ParsedPage p;
    p.page_num = 1;
    p.page_text = "content without paragraph structure here";
    auto r = ParentChildChunker().Chunk(MakeInput({p}));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().parents.size(), 1u);
    EXPECT_EQ(r.value().stats.succeeded_pages, 1u);
}

}  // namespace
}  // namespace cortrix::chunker
