#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/spc/docling_parser.h"
#include "cortrix/spc/parser.h"
#include "parser_script_fixture.h"

// Parser page-level progress + per-page tolerance (Q1, design S5). Standalone: the
// callback is driven by replaying the parsed ParsedDoc (real per-page streaming
// from the bridge is integration). DrivePageProgress is also tested directly.
namespace cortrix::spc {
namespace {

using test::ScriptFixture;

struct ProgressEvent {
    int page;
    int total;
    bool success;
};

// A 3-page success doc (pages 1,2,5) with failed_pages [3,4].
const char* kPartialDoc = R"JSON({
  "status": 0, "parser": "docling",
  "metadata": {"filename": "p.pdf", "page_count": 3},
  "pages": [
    {"page_num": 1, "page_text": "a", "page_metadata": {"page_num": 1, "char_count": 1},
     "paragraphs": [{"text": "a", "page": 1, "type": "TEXT"}]},
    {"page_num": 2, "page_text": "b", "page_metadata": {"page_num": 2, "char_count": 1},
     "paragraphs": [{"text": "b", "page": 2, "type": "TEXT"}]},
    {"page_num": 5, "page_text": "e", "page_metadata": {"page_num": 5, "char_count": 1},
     "paragraphs": [{"text": "e", "page": 5, "type": "TEXT"}]}],
  "failed_pages": [3, 4]})JSON";

class PageProgressTest : public ScriptFixture {
protected:
    DoclingParserConfig CfgFor(const std::string& script) {
        DoclingParserConfig cfg;
        cfg.script_path = script;
        cfg.subprocess_timeout_ms = 5000;
        return cfg;
    }
};

TEST_F(PageProgressTest, PartialFailure_CallbackPerPageInOrder) {
    DoclingParser p(CfgFor(MockEmitting(kPartialDoc)));
    std::vector<ProgressEvent> events;
    ParserOptions opts;
    opts.on_page_progress = [&](int page, int total, bool success) {
        events.push_back({page, total, success});
    };
    ParsedDoc doc = p.Parse("/tmp/p.pdf", opts);
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(doc.failed_pages, (std::vector<int>{3, 4}));

    // 3 ok + 2 failed = 5 callbacks, total=5, ascending page order.
    ASSERT_EQ(events.size(), 5u);
    EXPECT_EQ(events[0].page, 1); EXPECT_TRUE(events[0].success);
    EXPECT_EQ(events[1].page, 2); EXPECT_TRUE(events[1].success);
    EXPECT_EQ(events[2].page, 3); EXPECT_FALSE(events[2].success);  // failed
    EXPECT_EQ(events[3].page, 4); EXPECT_FALSE(events[3].success);  // failed
    EXPECT_EQ(events[4].page, 5); EXPECT_TRUE(events[4].success);
    for (const auto& e : events) EXPECT_EQ(e.total, 5);
}

TEST_F(PageProgressTest, AllSuccess_AllCallbacksSucceed) {
    const char* json = R"JSON({"status": 0, "parser": "docling",
      "metadata": {"filename": "p.pdf", "page_count": 2},
      "pages": [
        {"page_num": 1, "page_text": "a", "page_metadata": {"page_num": 1},
         "paragraphs": [{"text": "a", "page": 1, "type": "TEXT"}]},
        {"page_num": 2, "page_text": "b", "page_metadata": {"page_num": 2},
         "paragraphs": [{"text": "b", "page": 2, "type": "TEXT"}]}],
      "failed_pages": []})JSON";
    DoclingParser p(CfgFor(MockEmitting(json)));
    int count = 0;
    bool all_ok = true;
    ParserOptions opts;
    opts.on_page_progress = [&](int, int, bool s) { ++count; all_ok &= s; };
    ParsedDoc doc = p.Parse("/tmp/p.pdf", opts);
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(count, 2);
    EXPECT_TRUE(all_ok);
    EXPECT_TRUE(doc.failed_pages.empty());
}

TEST_F(PageProgressTest, NullCallback_NoCrash) {
    DoclingParser p(CfgFor(MockEmitting(kPartialDoc)));
    ParserOptions opts;  // on_page_progress = nullptr
    ParsedDoc doc = p.Parse("/tmp/p.pdf", opts);  // must not crash
    EXPECT_TRUE(doc.ok());
}

TEST_F(PageProgressTest, ErrorResult_NoCallback) {
    // A subprocess crash (error doc) → no per-page progress.
    DoclingParser p(CfgFor(Write(".py", "import sys\nsys.exit(1)\n")));
    int count = 0;
    ParserOptions opts;
    opts.on_page_progress = [&](int, int, bool) { ++count; };
    ParsedDoc doc = p.Parse("/tmp/p.pdf", opts);
    EXPECT_FALSE(doc.ok());
    EXPECT_EQ(count, 0);
}

// DrivePageProgress unit-level (no subprocess).
TEST(DrivePageProgressTest, MergesAndOrdersEvents) {
    ParsedDoc doc;
    doc.status = ParserErrorCode::kOk;
    ParsedPage p1; p1.page_num = 10;
    ParsedPage p2; p2.page_num = 1;
    doc.pages = {p1, p2};
    doc.failed_pages = {5};
    std::vector<int> pages;
    std::vector<bool> ok;
    ParserOptions opts;
    opts.on_page_progress = [&](int page, int total, bool success) {
        pages.push_back(page);
        ok.push_back(success);
        EXPECT_EQ(total, 3);
    };
    DrivePageProgress(doc, opts);
    EXPECT_EQ(pages, (std::vector<int>{1, 5, 10}));
    EXPECT_EQ(ok, (std::vector<bool>{true, false, true}));
}

TEST(DrivePageProgressTest, EmptyDocOkDrivesNoEvents) {
    ParsedDoc doc;
    doc.status = ParserErrorCode::kEmptyDocument;  // OK, no pages
    int count = 0;
    ParserOptions opts;
    opts.on_page_progress = [&](int, int, bool) { ++count; };
    DrivePageProgress(doc, opts);
    EXPECT_EQ(count, 0);
}

}  // namespace
}  // namespace cortrix::spc
