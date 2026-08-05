#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/import/spc_feed.h"
#include "cortrix/import/text_serializer.h"

// S3 coverage: TextSerializer D4 strategies (PER_ROW / MERGE — no template, DB import
// §3.5 v1.0.2 resolution 13), the §4.3 Block source URI + metadata, the D5 SPC-feed seam,
// and the D3 full-overwrite prefix matching (incl. the "users/ ≠ users2/" boundary).
namespace cortrix::import {
namespace {

DbRow MakeRow(const std::string& id, std::vector<std::pair<std::string, std::string>> cols) {
    DbRow r;
    r.row_id = id;
    for (auto& [k, v] : cols) r.columns.push_back({k, DbValue{v, false}});
    return r;
}

SourceContext MakeSrc(const std::string& table = "users") {
    SourceContext s;
    s.host = "db.internal";
    s.port = 5432;
    s.db_name = "crm";
    s.table = table;
    s.connection_ref = "db_conn_abc";
    s.import_task_id = "import_xyz";
    s.query_signature = "users:status=active";
    s.imported_by = "admin1";
    s.imported_at_iso = "2026-05-31T10:30:00Z";
    return s;
}

// --- source URI + prefix (§4.3 / ARCH §5.1) ---

TEST(TextSerializerSourceTest, BuildsPostgresSourceUri) {
    auto src = MakeSrc();
    EXPECT_EQ(BuildSourceUri(src, "42"),
              "postgres://db.internal:5432/crm/users/42");
}

TEST(TextSerializerSourceTest, PrefixHasTrailingSlashBoundary) {
    auto src = MakeSrc("users");
    std::string prefix = SourcePrefix(src);
    EXPECT_EQ(prefix, "postgres://db.internal:5432/crm/users/");
    // the trailing '/' guarantees "users/" never matches a "users2/" URI.
    std::string users2_uri = BuildSourceUri(MakeSrc("users2"), "1");
    EXPECT_NE(users2_uri.rfind(prefix, 0), 0u);
}

// --- PER_ROW ---

TEST(TextSerializerTest, PerRowOneChunkPerRow) {
    TextSerializer ts;
    std::vector<DbRow> rows = {
        MakeRow("1", {{"id", "1"}, {"name", "Ada"}}),
        MakeRow("2", {{"id", "2"}, {"name", "Bob"}}),
    };
    auto chunks = ts.Serialize(rows, TextStrategy::kPerRow, MakeSrc());
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0].text, "id: 1\nname: Ada\n");
    EXPECT_EQ(chunks[0].source, "postgres://db.internal:5432/crm/users/1");
    EXPECT_EQ(chunks[0].metadata["source_type"], "database_import");
    EXPECT_EQ(chunks[0].metadata["source_connection_ref"], "db_conn_abc");
    EXPECT_EQ(chunks[0].metadata["import_task_id"], "import_xyz");
    EXPECT_EQ(chunks[0].metadata["source"], "postgres://db.internal:5432/crm/users/1");
    EXPECT_EQ(chunks[1].source, "postgres://db.internal:5432/crm/users/2");
}

TEST(TextSerializerTest, PerRowRendersNullColumnAsEmpty) {
    TextSerializer ts;
    DbRow r;
    r.row_id = "1";
    r.columns = {{"id", DbValue{"1", false}}, {"bio", DbValue{"", true}}};
    auto chunks = ts.Serialize({r}, TextStrategy::kPerRow, MakeSrc());
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].text, "id: 1\nbio: \n");  // null → present but empty (keeps shape)
}

// --- MERGE ---

TEST(TextSerializerTest, MergeFoldsRowsByMergeSize) {
    TextSerializer ts;
    std::vector<DbRow> rows;
    for (int i = 1; i <= 5; ++i) {
        rows.push_back(MakeRow(std::to_string(i), {{"id", std::to_string(i)}}));
    }
    auto chunks = ts.Serialize(rows, TextStrategy::kMerge, MakeSrc(), /*merge_size=*/2);
    ASSERT_EQ(chunks.size(), 3u);  // ceil(5/2)
    EXPECT_EQ(chunks[0].metadata["merged_row_count"], 2);
    EXPECT_EQ(chunks[2].metadata["merged_row_count"], 1);  // last chunk = 1 row
    EXPECT_NE(chunks[0].text.find("\n---\n"), std::string::npos);  // row separator
    // merged chunk anchors its source to the first row id in the batch.
    EXPECT_EQ(chunks[0].source, "postgres://db.internal:5432/crm/users/1");
    EXPECT_EQ(chunks[1].source, "postgres://db.internal:5432/crm/users/3");
}

TEST(TextSerializerTest, EmptyRowsYieldNoChunks) {
    TextSerializer ts;
    EXPECT_TRUE(ts.Serialize({}, TextStrategy::kPerRow, MakeSrc()).empty());
    EXPECT_TRUE(ts.Serialize({}, TextStrategy::kMerge, MakeSrc()).empty());
}

// --- D5 SPC feed seam ---

TEST(SpcFeedTest, FeederRecordsChunksAndCounts) {
    InMemorySpcFeeder feeder;
    TextSerializer ts;
    auto chunks = ts.Serialize({MakeRow("1", {{"id", "1"}})}, TextStrategy::kPerRow, MakeSrc());
    auto r = feeder.Feed(chunks, "customer_kb");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 1);
    EXPECT_EQ(feeder.total_chunks(), 1);
    ASSERT_EQ(feeder.batches().size(), 1u);
    EXPECT_EQ(feeder.batches()[0].ns, "customer_kb");
    EXPECT_EQ(feeder.batches()[0].chunks[0].source, "postgres://db.internal:5432/crm/users/1");
}

// --- D3 full-overwrite prefix matching (ARCH §3.6: only this table's Blocks) ---

TEST(BlockCleanerTest, ClearsOnlyMatchingTablePrefix) {
    InMemoryBlockCleaner cleaner;
    cleaner.SeedBlock("ns1", "postgres://db.internal:5432/crm/users/1");
    cleaner.SeedBlock("ns1", "postgres://db.internal:5432/crm/users/2");
    cleaner.SeedBlock("ns1", "postgres://db.internal:5432/crm/orders/9");  // other table
    cleaner.SeedBlock("ns1", "file:///docs/readme.md");                    // other source type

    auto src = MakeSrc("users");
    auto r = cleaner.CleanupSourceBlocks("ns1", SourcePrefix(src));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 2);                  // only the 2 users Blocks
    EXPECT_EQ(cleaner.RemainingCount("ns1"), 2);  // orders + file survive
}

TEST(BlockCleanerTest, TableBoundaryDoesNotClearSiblingPrefix) {
    InMemoryBlockCleaner cleaner;
    cleaner.SeedBlock("ns1", "postgres://db.internal:5432/crm/users/1");
    cleaner.SeedBlock("ns1", "postgres://db.internal:5432/crm/users2/1");  // sibling table

    auto r = cleaner.CleanupSourceBlocks("ns1", SourcePrefix(MakeSrc("users")));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 1);                      // only users/, NOT users2/
    EXPECT_EQ(cleaner.RemainingCount("ns1"), 1);  // users2 survives
}

TEST(BlockCleanerTest, NamespaceScoped) {
    InMemoryBlockCleaner cleaner;
    cleaner.SeedBlock("ns1", "postgres://db.internal:5432/crm/users/1");
    cleaner.SeedBlock("ns2", "postgres://db.internal:5432/crm/users/1");  // same uri, other ns
    auto r = cleaner.CleanupSourceBlocks("ns1", SourcePrefix(MakeSrc("users")));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 1);
    EXPECT_EQ(cleaner.RemainingCount("ns2"), 1);  // ns2 untouched
}

}  // namespace
}  // namespace cortrix::import
