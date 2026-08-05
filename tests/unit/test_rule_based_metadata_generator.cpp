#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/metadata/metadata_error.h"
#include "cortrix/metadata/metadata_metrics.h"
#include "cortrix/metadata/metadata_types.h"
#include "cortrix/metadata/rule_based_metadata_generator.h"

// META block S1.2 / S2.4 / §7.2 coverage: RuleBasedMetadataGenerator — the core block_text
// assembly + 26-field metadata_json serialization (≥95% target), the three metadata
// cases (full input / missing page_count / parser total failure), and the §9.quater.2
// doc_fts5 derived mapping.
namespace cortrix::metadata {
namespace {

// A fully-populated, "happy path" input (case 1). 2026-04-02T10:30:00Z = 1775125800000 ms.
GeneratorInput FullInput() {
    GeneratorInput in;
    in.doc_id = "01HXXDOC";
    in.namespace_id = "default";

    auto& dm = in.doc_metadata;
    dm.filename = "quarterly_report.pdf";
    dm.doc_title = "Quarterly Report 2026 Q1";
    dm.mime_type = "application/pdf";
    dm.file_size_bytes = 2048576;
    dm.page_count = 15;
    dm.doc_language = "zh";
    dm.upload_timestamp = 0;  // upload_time is carried in FileInfo (request-level)

    in.file_info.source_uri = "file:///uploads/quarterly_report.pdf";
    in.file_info.tags = {"financial", "Q1-2026"};
    in.file_info.upload_time_ms = 1775125800000LL;

    auto& st = in.stats;
    st.page_count = 15;
    st.chunk_count = 42;
    st.parent_count = 5;
    st.child_count = 42;
    st.processing_time_ms = 3200;
    st.parser_name = "docling";
    st.parser_version = "1.2.0";
    st.enricher_name = "llm";
    st.enricher_version = "1.0";
    st.chunker_name = "parent-child";
    st.chunker_version = "1.0";
    st.parse_status = "ok";
    st.parse_failed_page = -1;

    in.custom_metadata = {{"owner", "finance-team"},
                          {"compliance", "pre-audit"},
                          {"review_status", "pending"}};
    return in;
}

// --- 26-field schema completeness (D2/D9 + V2 rework §3.1) ---

TEST(MetadataGeneratorTest, FullInputProducesAll26SchemaFields) {
    std::vector<std::string> missing;
    nlohmann::json j = RuleBasedMetadataGenerator::BuildMetadataJson(FullInput(), &missing);

    // Every §3.1 key is present (the schema must not silently shrink). Listed
    // explicitly so the test documents the locked 26-field set.
    static const std::vector<std::string> kSchema = {
        "block_id", "block_type", "doc_id", "namespace_id",
        "filename", "mime_type", "file_size_bytes", "upload_time",
        "page_count", "chunk_count", "parent_count", "child_count",
        "parser", "parser_version", "enricher", "enricher_version",
        "chunker", "chunker_version",
        "processing_time_ms", "ingestion_status",
        "source_uri", "tags", "lang",
        "meta.parse_status", "meta.parse_failed_page", "custom_metadata"};
    EXPECT_EQ(kSchema.size(), static_cast<size_t>(kMetadataFieldCount));
    for (const auto& key : kSchema) {
        EXPECT_TRUE(j.contains(key)) << "missing schema key: " << key;
    }
    // No extra keys beyond the locked set.
    EXPECT_EQ(j.size(), static_cast<size_t>(kMetadataFieldCount));
    EXPECT_TRUE(missing.empty());
}

TEST(MetadataGeneratorTest, FullInputFieldValuesMatchInput) {
    std::vector<std::string> missing;
    nlohmann::json j = RuleBasedMetadataGenerator::BuildMetadataJson(FullInput(), &missing);

    EXPECT_EQ(j["block_type"], "metadata");
    EXPECT_EQ(j["doc_id"], "01HXXDOC");
    EXPECT_EQ(j["namespace_id"], "default");
    EXPECT_EQ(j["filename"], "quarterly_report.pdf");
    EXPECT_EQ(j["mime_type"], "application/pdf");
    EXPECT_EQ(j["file_size_bytes"], 2048576);
    EXPECT_EQ(j["upload_time"], "2026-04-02T10:30:00Z");  // ISO-8601 UTC from epoch ms
    EXPECT_EQ(j["page_count"], 15);
    EXPECT_EQ(j["chunk_count"], 42);
    EXPECT_EQ(j["parent_count"], 5);
    EXPECT_EQ(j["child_count"], 42);
    EXPECT_EQ(j["parser"], "docling");
    EXPECT_EQ(j["parser_version"], "1.2.0");
    EXPECT_EQ(j["enricher"], "llm");
    EXPECT_EQ(j["chunker"], "parent-child");
    EXPECT_EQ(j["processing_time_ms"], 3200);
    EXPECT_EQ(j["source_uri"], "file:///uploads/quarterly_report.pdf");
    EXPECT_EQ(j["lang"], "zh");  // ← parser doc_language (consumed-field name difference)
    EXPECT_EQ(j["tags"], (nlohmann::json{"financial", "Q1-2026"}));
    EXPECT_EQ(j["ingestion_status"], "completed");
    EXPECT_EQ(j["meta.parse_status"], "ok");
    EXPECT_TRUE(j["meta.parse_failed_page"].is_null());
    EXPECT_EQ(j["custom_metadata"]["owner"], "finance-team");
}

// Case 2: page_count unknown → null + recorded in missing_fields (§5.2 row 2 / §5.3).
TEST(MetadataGeneratorTest, MissingPageCountNullsAndWarns) {
    GeneratorInput in = FullInput();
    in.doc_metadata.page_count = -1;  // Parser couldn't determine page count

    std::vector<std::string> missing;
    nlohmann::json j = RuleBasedMetadataGenerator::BuildMetadataJson(in, &missing);

    EXPECT_TRUE(j["page_count"].is_null());
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0], "page_count");
    // Schema is still complete (page_count key present, just null).
    EXPECT_EQ(j.size(), static_cast<size_t>(kMetadataFieldCount));
}

// Unknown counts from parent-child chunking (standalone: not yet supplied) → null, schema still complete.
TEST(MetadataGeneratorTest, UnknownCountsBecomeNull) {
    GeneratorInput in = FullInput();
    in.stats.chunk_count = -1;
    in.stats.parent_count = -1;
    in.stats.child_count = -1;

    std::vector<std::string> missing;
    nlohmann::json j = RuleBasedMetadataGenerator::BuildMetadataJson(in, &missing);
    EXPECT_TRUE(j["chunk_count"].is_null());
    EXPECT_TRUE(j["parent_count"].is_null());
    EXPECT_TRUE(j["child_count"].is_null());
}

// Cleaning coordination (V2 rework M-03): parse_status=partial + a failed page → meta.parse_failed_page
// non-null; ingestion_status follows the status (failed → "failed").
TEST(MetadataGeneratorTest, ParseStatusF10Signals) {
    GeneratorInput in = FullInput();
    in.stats.parse_status = "partial";
    in.stats.parse_failed_page = 7;
    std::vector<std::string> missing;
    nlohmann::json j = RuleBasedMetadataGenerator::BuildMetadataJson(in, &missing);
    EXPECT_EQ(j["meta.parse_status"], "partial");
    EXPECT_EQ(j["meta.parse_failed_page"], 7);
    EXPECT_EQ(j["ingestion_status"], "completed");  // partial still completed (only failed→failed)

    in.stats.parse_status = "failed";
    j = RuleBasedMetadataGenerator::BuildMetadataJson(in, &missing);
    EXPECT_EQ(j["ingestion_status"], "failed");

    // failed status but no page recorded → null page.
    in.stats.parse_failed_page = -1;
    j = RuleBasedMetadataGenerator::BuildMetadataJson(in, &missing);
    EXPECT_TRUE(j["meta.parse_failed_page"].is_null());
}

// Empty parse_status normalizes to "ok" (§3.1 required enum; never empty on the wire).
TEST(MetadataGeneratorTest, EmptyParseStatusNormalizesToOk) {
    GeneratorInput in = FullInput();
    in.stats.parse_status = "";
    std::vector<std::string> missing;
    nlohmann::json j = RuleBasedMetadataGenerator::BuildMetadataJson(in, &missing);
    EXPECT_EQ(j["meta.parse_status"], "ok");
}

// custom_metadata null/absent → normalized to {} (never JSON null in the schema).
TEST(MetadataGeneratorTest, NullCustomMetadataBecomesEmptyObject) {
    GeneratorInput in = FullInput();
    in.custom_metadata = nlohmann::json();  // null
    std::vector<std::string> missing;
    nlohmann::json j = RuleBasedMetadataGenerator::BuildMetadataJson(in, &missing);
    ASSERT_TRUE(j["custom_metadata"].is_object());
    EXPECT_TRUE(j["custom_metadata"].empty());
}

// --- block_text (D3 locked: natural-language sentence, §3.2) ---

TEST(MetadataGeneratorTest, BlockTextContainsCoreFieldsOnly) {
    std::string text = RuleBasedMetadataGenerator::BuildBlockText(FullInput());

    // Core fields (§3.2): filename / mime_type / page_count / upload_time / lang / tags / parser.
    EXPECT_NE(text.find("quarterly_report.pdf"), std::string::npos);
    EXPECT_NE(text.find("application/pdf"), std::string::npos);
    // "15 pages" = page-count suffix emitted by production BuildBlockText
    // (src/metadata/rule_based_metadata_generator.cpp). Display label, English
    // per the 2026-06-01 English-ization; kept in sync with that source.
    EXPECT_NE(text.find("15 pages"), std::string::npos);
    EXPECT_NE(text.find("2026-04-02T10:30:00Z"), std::string::npos);
    EXPECT_NE(text.find("zh"), std::string::npos);
    EXPECT_NE(text.find("financial"), std::string::npos);
    EXPECT_NE(text.find("docling"), std::string::npos);

    // NOT in block_text (§3.2): block_id / doc_id / namespace_id / *_version /
    // file_size / business custom_metadata.
    EXPECT_EQ(text.find("01HXXDOC"), std::string::npos);       // doc_id
    EXPECT_EQ(text.find("default"), std::string::npos);        // namespace_id
    EXPECT_EQ(text.find("1.2.0"), std::string::npos);          // parser_version
    EXPECT_EQ(text.find("2048576"), std::string::npos);        // file_size_bytes
    EXPECT_EQ(text.find("finance-team"), std::string::npos);   // custom_metadata
}

// block_text is deterministic (embedding reproducibility) and skips absent clauses.
TEST(MetadataGeneratorTest, BlockTextDeterministicAndSkipsEmpty) {
    GeneratorInput in = FullInput();
    EXPECT_EQ(RuleBasedMetadataGenerator::BuildBlockText(in),
              RuleBasedMetadataGenerator::BuildBlockText(in));

    GeneratorInput sparse;
    sparse.doc_metadata.filename = "notes.txt";
    // no mime_type / page_count / upload / lang / tags / parser
    std::string text = RuleBasedMetadataGenerator::BuildBlockText(sparse);
    EXPECT_EQ(text, "notes.txt");  // only the present clause, no empty-field noise
}

// --- Generate() end-to-end ---

TEST(MetadataGeneratorTest, GenerateSucceedsAndMintsUlidBlockId) {
    RuleBasedMetadataGenerator gen;
    auto res = gen.Generate(FullInput());
    ASSERT_TRUE(res.ok()) << res.status().message();

    const MetadataBlock& blk = res.value().block;
    EXPECT_EQ(blk.doc_id, "01HXXDOC");
    EXPECT_EQ(blk.namespace_id, "default");
    EXPECT_EQ(blk.block_id.size(), 26u);  // ULID is 26-char
    // block_id is reflected into the JSON too.
    EXPECT_EQ(blk.metadata_json["block_id"], blk.block_id);
    EXPECT_FALSE(blk.block_text.empty());
    // embedding(block_text) is D3.5 pipeline wiring → empty standalone.
    EXPECT_TRUE(blk.embedding.empty());
    EXPECT_TRUE(res.value().missing_fields.empty());
}

// Two generations mint distinct block_ids (ULID uniqueness).
TEST(MetadataGeneratorTest, GenerateMintsDistinctBlockIds) {
    RuleBasedMetadataGenerator gen;
    auto a = gen.Generate(FullInput());
    auto b = gen.Generate(FullInput());
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_NE(a.value().block.block_id, b.value().block.block_id);
}

TEST(MetadataGeneratorTest, GeneratePartialSurfacesMissingFields) {
    GeneratorInput in = FullInput();
    in.doc_metadata.page_count = -1;
    RuleBasedMetadataGenerator gen;
    auto res = gen.Generate(in);
    ASSERT_TRUE(res.ok());  // partial is still HTTP 200 (§5.2)
    ASSERT_EQ(res.value().missing_fields.size(), 1u);
    EXPECT_EQ(res.value().missing_fields[0], "page_count");
}

// Case 3: parser total failure (no filename, no source_uri, parse_status=failed) → GEN_FAILED.
TEST(MetadataGeneratorTest, GenerateFailsWhenF06ParseCompletelyFailed) {
    GeneratorInput in;
    in.doc_id = "01HXXDOC";
    in.namespace_id = "default";
    in.stats.parse_status = "failed";
    // doc_metadata empty (filename ""), file_info.source_uri empty.

    RuleBasedMetadataGenerator gen;
    auto res = gen.Generate(in);
    ASSERT_FALSE(res.ok());
    EXPECT_EQ(res.status().code(), StatusCode::kInternal);
    EXPECT_NE(res.status().message().find("CX_ERR_METADATA_GEN_FAILED"), std::string::npos);
}

// A failed parse that still has identity (filename present) is NOT a hard failure —
// the block is generated as an L0 fallback (§5.2: only "completely empty" fails).
TEST(MetadataGeneratorTest, FailedParseWithFilenameStillGenerates) {
    GeneratorInput in = FullInput();
    in.stats.parse_status = "failed";
    RuleBasedMetadataGenerator gen;
    auto res = gen.Generate(in);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res.value().block.metadata_json["ingestion_status"], "failed");
}

// §5.bis cortrix_f08_block_generate_duration_seconds feed point: each produced-block
// Generate() must observe the histogram (the recorder method existed but Generate()
// never fed it). Mirrors the semantic score AssignFeedsDurationHistogram regression — lock that
// each generating call increments _count by 1.
TEST(MetadataGeneratorTest, GenerateFeedsDurationHistogram) {
    MetadataMetrics::Instance().ResetForTest();
    RuleBasedMetadataGenerator gen;
    ASSERT_TRUE(gen.Generate(FullInput()).ok());
    GeneratorInput partial = FullInput();
    partial.doc_metadata.page_count = -1;  // partial_warning path still produces a block
    ASSERT_TRUE(gen.Generate(partial).ok());

    const std::string out = MetadataMetrics::Instance().Render();
    EXPECT_NE(out.find("cortrix_f08_block_generate_duration_seconds_count 2"), std::string::npos)
        << out;
    MetadataMetrics::Instance().ResetForTest();
}

// The GEN_FAILED early return does no block assembly → it must NOT feed the duration
// histogram (the timer starts after the failure guard). Locks that boundary.
TEST(MetadataGeneratorTest, GenerateFailureDoesNotFeedDurationHistogram) {
    MetadataMetrics::Instance().ResetForTest();
    GeneratorInput in;
    in.doc_id = "01HXXDOC";
    in.namespace_id = "default";
    in.stats.parse_status = "failed";  // no identity + failed → GEN_FAILED
    RuleBasedMetadataGenerator gen;
    ASSERT_FALSE(gen.Generate(in).ok());

    const std::string out = MetadataMetrics::Instance().Render();
    EXPECT_NE(out.find("cortrix_f08_block_generate_duration_seconds_count 0"), std::string::npos)
        << out;
    MetadataMetrics::Instance().ResetForTest();
}

// --- §9.quater.2 doc_fts5 derived mapping ---

TEST(MetadataGeneratorTest, DeriveDocFts5Columns) {
    nlohmann::json cols = RuleBasedMetadataGenerator::DeriveDocFts5Columns(FullInput());
    EXPECT_EQ(cols["doc_id"], "01HXXDOC");
    EXPECT_EQ(cols["filename"], "quarterly_report.pdf");
    EXPECT_EQ(cols["doc_title"], "quarterly_report");          // filename with extension stripped
    EXPECT_EQ(cols["topics_rule_extracted"], "financial Q1-2026");  // tags.join(' ')
    EXPECT_TRUE(cols["authors"].is_null());                    // custom_metadata.authors not filled

    // custom_metadata.authors present → mapped through.
    GeneratorInput in = FullInput();
    in.custom_metadata["authors"] = "finance-team";
    cols = RuleBasedMetadataGenerator::DeriveDocFts5Columns(in);
    EXPECT_EQ(cols["authors"], "finance-team");
}

// doc_title derivation edge cases: no-dot / leading-dot names returned unchanged.
TEST(MetadataGeneratorTest, DeriveDocTitleEdgeCases) {
    GeneratorInput in = FullInput();
    in.doc_metadata.filename = "README";  // no extension
    EXPECT_EQ(RuleBasedMetadataGenerator::DeriveDocFts5Columns(in)["doc_title"], "README");
    in.doc_metadata.filename = ".gitignore";  // leading dot only
    EXPECT_EQ(RuleBasedMetadataGenerator::DeriveDocFts5Columns(in)["doc_title"], ".gitignore");
    in.doc_metadata.filename = "a.b.c.pdf";  // last extension only
    EXPECT_EQ(RuleBasedMetadataGenerator::DeriveDocFts5Columns(in)["doc_title"], "a.b.c");
}

}  // namespace
}  // namespace cortrix::metadata
