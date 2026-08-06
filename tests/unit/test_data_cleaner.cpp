#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/catalog/config_resolver.h"
#include "cortrix/common/block_types.h"
#include "cortrix/spc/data_cleaner.h"

// Cleaning DataCleaner: Dedup (exact hash + semantic cosine,/), DetectAnomaly
// (5 reasons), config validation, NS config merge (reuse catalog
// ConfigResolver), and the skip-index helper. Standalone over the cleaning
// `Block` contract view.
namespace cortrix::spc {
namespace {

// A block with text + an embedding (a unit vector pointing along axis `axis`,
// scaled by `mag`, in `dim` dims) — handy for controlling cosine similarity.
Block MakeBlock(const std::string& id, const std::string& text,
                int dim = 4, int axis = 0, float mag = 1.0f) {
    Block b;
    b.id = id;
    b.chunk_text = text;
    b.embedding.assign(dim, 0.0f);
    if (axis >= 0 && axis < dim) b.embedding[axis] = mag;
    return b;
}

CleaningConfig DefaultCfg() { return CleaningConfig{}; }  // dedup+anomaly on, 0.95

// ---------- Dedup: exact hash (Step 1) ----------

TEST(DataCleanerDedupTest, ExactDup_SameTextRemoved) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {
        MakeBlock("a", "hello world", 4, 0),
        MakeBlock("b", "hello world", 4, 1),  // identical text, different vec
        MakeBlock("c", "different",   4, 2),
    };
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.hash_dedup_count, 1);
    EXPECT_EQ(r.removed_count, 1);
    EXPECT_EQ(blocks.size(), 2u);
    EXPECT_EQ(r.removed_block_ids, (std::vector<std::string>{"b"}));
}

TEST(DataCleanerDedupTest, AllDistinct_NothingRemoved) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {
        MakeBlock("a", "one",   4, 0),
        MakeBlock("b", "two",   4, 1),
        MakeBlock("c", "three", 4, 2),
    };
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.removed_count, 0);
    EXPECT_EQ(r.kept_count, 3);
    EXPECT_EQ(blocks.size(), 3u);
}

TEST(DataCleanerDedupTest, AllSameText_KeepsOne) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {
        MakeBlock("a", "dup", 4, 0), MakeBlock("b", "dup", 4, 1),
        MakeBlock("c", "dup", 4, 2), MakeBlock("d", "dup", 4, 3),
    };
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.hash_dedup_count, 3);
    EXPECT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].id, "a");  // first survives
}

TEST(DataCleanerDedupTest, EmptyInput) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks;
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.removed_count, 0);
    EXPECT_EQ(r.kept_count, 0);
}

TEST(DataCleanerDedupTest, SingleElement) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {MakeBlock("a", "solo", 4, 0)};
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.removed_count, 0);
    EXPECT_EQ(blocks.size(), 1u);
}

TEST(DataCleanerDedupTest, Disabled_NoDedup) {
    CleaningConfig cfg = DefaultCfg();
    cfg.dedup_enabled = false;
    DataCleaner c(cfg);
    std::vector<Block> blocks = {MakeBlock("a", "x", 4, 0), MakeBlock("b", "x", 4, 0)};
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.removed_count, 0);
    EXPECT_EQ(blocks.size(), 2u);  // identical text kept — dedup off
    EXPECT_EQ(r.kept_count, 2);
}

// ---------- Dedup: semantic cosine (Step 2) ----------

TEST(DataCleanerDedupTest, Semantic_IdenticalVectorsRemoved) {
    DataCleaner c(DefaultCfg());  // threshold 0.95
    std::vector<Block> blocks = {
        MakeBlock("a", "text one", 4, 0, 1.0f),  // distinct text...
        MakeBlock("b", "text two", 4, 0, 2.0f),  // ...but parallel vector (cos=1.0)
    };
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.hash_dedup_count, 0);     // texts differ → no exact dup
    EXPECT_EQ(r.semantic_dedup_count, 1); // vectors parallel → semantic dup
    EXPECT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].id, "a");
}

TEST(DataCleanerDedupTest, Semantic_OrthogonalVectorsKept) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {
        MakeBlock("a", "t1", 4, 0),   // axis 0
        MakeBlock("b", "t2", 4, 1),   // axis 1 → cos 0
    };
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.semantic_dedup_count, 0);
    EXPECT_EQ(blocks.size(), 2u);
}

TEST(DataCleanerDedupTest, Semantic_ThresholdBoundary) {
    // cos = 0.8 between two vectors; threshold 0.85 keeps, 0.75 drops.
    auto two = []() {
        Block a; a.id = "a"; a.chunk_text = "t1"; a.embedding = {1.0f, 0.0f};
        Block b; b.id = "b"; b.chunk_text = "t2"; b.embedding = {0.8f, 0.6f};  // cos=0.8
        return std::vector<Block>{a, b};
    };
    {
        CleaningConfig cfg = DefaultCfg(); cfg.dedup_similarity_threshold = 0.85;
        DataCleaner c(cfg);
        auto blocks = two();
        DedupResult r = c.Dedup(blocks);
        EXPECT_EQ(r.semantic_dedup_count, 0);  // 0.8 < 0.85 → kept
        EXPECT_EQ(blocks.size(), 2u);
    }
    {
        CleaningConfig cfg = DefaultCfg(); cfg.dedup_similarity_threshold = 0.75;
        DataCleaner c(cfg);
        auto blocks = two();
        DedupResult r = c.Dedup(blocks);
        EXPECT_EQ(r.semantic_dedup_count, 1);  // 0.8 >= 0.75 → dropped
        EXPECT_EQ(blocks.size(), 1u);
    }
}

TEST(DataCleanerDedupTest, Semantic_NaNAndZeroVectorsNotSimilar) {
    DataCleaner c(DefaultCfg());
    Block z1; z1.id = "z1"; z1.chunk_text = "a"; z1.embedding = {0.0f, 0.0f};
    Block z2; z2.id = "z2"; z2.chunk_text = "b"; z2.embedding = {0.0f, 0.0f};
    Block nan; nan.id = "n"; nan.chunk_text = "c";
    nan.embedding = {std::nanf(""), 1.0f};
    std::vector<Block> blocks = {z1, z2, nan};
    DedupResult r = c.Dedup(blocks);
    // Zero/NaN vectors yield cosine 0 → never deduped semantically (they are
    // flagged by DetectAnomaly instead).
    EXPECT_EQ(r.semantic_dedup_count, 0);
    EXPECT_EQ(blocks.size(), 3u);
}

TEST(DataCleanerDedupTest, Semantic_MultiplePairsCollapse) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {
        MakeBlock("a", "t1", 4, 0, 1.0f),
        MakeBlock("b", "t2", 4, 0, 3.0f),  // parallel to a
        MakeBlock("c", "t3", 4, 0, 5.0f),  // parallel to a
        MakeBlock("d", "t4", 4, 1, 1.0f),  // orthogonal → survives
    };
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.semantic_dedup_count, 2);  // b, c collapse into a
    EXPECT_EQ(blocks.size(), 2u);
}

TEST(DataCleanerDedupTest, HashThenSemantic_Combined) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {
        MakeBlock("a", "same", 4, 0, 1.0f),
        MakeBlock("b", "same", 4, 1, 1.0f),  // exact dup of a (text) → hash removed
        MakeBlock("c", "other", 4, 0, 2.0f), // parallel to a's vec → semantic dup
    };
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.hash_dedup_count, 1);
    EXPECT_EQ(r.semantic_dedup_count, 1);
    EXPECT_EQ(r.removed_count, 2);
    EXPECT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].id, "a");
}

// ---------- DetectAnomaly (5 reasons) ----------

TEST(DataCleanerAnomalyTest, EmptyChunk) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {MakeBlock("a", "", 4, 0)};
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.total_marked, 1);
    EXPECT_EQ(r.by_reason["empty"], 1);
    EXPECT_TRUE(blocks[0].flags_ext & kFlagExtIsAnomalous);
    EXPECT_EQ(blocks[0].metadata_json["cleaning.anomaly_reason"], "empty");
    EXPECT_EQ(blocks[0].metadata_json["cleaning.skip_index"], true);
}

TEST(DataCleanerAnomalyTest, OversizedChunk) {
    CleaningConfig cfg = DefaultCfg(); cfg.max_chunk_chars = 5;
    DataCleaner c(cfg);
    std::vector<Block> blocks = {MakeBlock("a", "0123456789", 4, 0)};  // len 10 > 5
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.by_reason["oversized"], 1);
    EXPECT_TRUE(blocks[0].flags_ext & kFlagExtIsAnomalous);
}

TEST(DataCleanerAnomalyTest, ParseFailed_FromMetaStatus) {
    DataCleaner c(DefaultCfg());
    Block b = MakeBlock("a", "text", 4, 0);
    b.metadata_json["meta.parse_status"] = "failed";
    std::vector<Block> blocks = {b};
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.by_reason["parse_failed"], 1);
}

TEST(DataCleanerAnomalyTest, ParseFailed_FromFailedPage) {
    DataCleaner c(DefaultCfg());
    Block b = MakeBlock("a", "text", 4, 0);
    b.metadata_json["meta.parse_failed_page"] = 3;
    std::vector<Block> blocks = {b};
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.by_reason["parse_failed"], 1);
}

TEST(DataCleanerAnomalyTest, InvalidEmbedding_NaN) {
    DataCleaner c(DefaultCfg());
    Block b; b.id = "a"; b.chunk_text = "text"; b.embedding = {1.0f, std::nanf("")};
    std::vector<Block> blocks = {b};
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.by_reason["invalid_embedding"], 1);
}

TEST(DataCleanerAnomalyTest, InvalidEmbedding_AllZero) {
    DataCleaner c(DefaultCfg());
    Block b; b.id = "a"; b.chunk_text = "text"; b.embedding = {0.0f, 0.0f, 0.0f};
    std::vector<Block> blocks = {b};
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.by_reason["invalid_embedding"], 1);
}

TEST(DataCleanerAnomalyTest, DuplicateBlockId) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {
        MakeBlock("dup", "t1", 4, 0),
        MakeBlock("dup", "t2", 4, 1),  // same id → second flagged
    };
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.by_reason["duplicate_id"], 1);
    EXPECT_FALSE(blocks[0].flags_ext & kFlagExtIsAnomalous);
    EXPECT_TRUE(blocks[1].flags_ext & kFlagExtIsAnomalous);
}

TEST(DataCleanerAnomalyTest, CompositeReasons) {
    CleaningConfig cfg = DefaultCfg(); cfg.max_chunk_chars = 3;
    DataCleaner c(cfg);
    // Oversized + invalid embedding on one block.
    Block b; b.id = "a"; b.chunk_text = "toolong"; b.embedding = {0.0f, 0.0f};
    std::vector<Block> blocks = {b};
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.total_marked, 1);
    const std::string reason = blocks[0].metadata_json["cleaning.anomaly_reason"];
    EXPECT_NE(reason.find("oversized"), std::string::npos);
    EXPECT_NE(reason.find("invalid_embedding"), std::string::npos);
    EXPECT_EQ(r.by_reason["oversized"], 1);
    EXPECT_EQ(r.by_reason["invalid_embedding"], 1);
}

TEST(DataCleanerAnomalyTest, CleanBlock_NotMarked) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {MakeBlock("a", "perfectly fine text", 4, 0)};
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.total_marked, 0);
    EXPECT_FALSE(blocks[0].flags_ext & kFlagExtIsAnomalous);
    EXPECT_FALSE(blocks[0].metadata_json.contains("cleaning.skip_index"));
}

TEST(DataCleanerAnomalyTest, Disabled_NoMarking) {
    CleaningConfig cfg = DefaultCfg(); cfg.anomaly_detection_enabled = false;
    DataCleaner c(cfg);
    std::vector<Block> blocks = {MakeBlock("a", "", 4, 0)};  // would be empty-anomaly
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.total_marked, 0);
    EXPECT_FALSE(blocks[0].flags_ext & kFlagExtIsAnomalous);
}

// ---------- skip-index helper ----------

TEST(DataCleanerSkipIndexTest, ShouldSkipIndex) {
    DataCleaner c(DefaultCfg());
    std::vector<Block> blocks = {MakeBlock("a", "", 4, 0),       // anomalous (empty)
                                 MakeBlock("b", "good text", 4, 0)};
    c.DetectAnomaly(blocks);
    EXPECT_TRUE(ShouldSkipIndex(blocks[0]));   // flagged
    EXPECT_FALSE(ShouldSkipIndex(blocks[1]));  // clean → indexed
}

// ---------- config validation ----------

TEST(DataCleanerConfigTest, ValidConfigPasses) {
    DataCleaner c(DefaultCfg());
    EXPECT_TRUE(c.ValidateConfig().ok());
}

TEST(DataCleanerConfigTest, ThresholdOutOfRange) {
    CleaningConfig hi = DefaultCfg(); hi.dedup_similarity_threshold = 1.5;
    EXPECT_FALSE(DataCleaner(hi).ValidateConfig().ok());
    CleaningConfig lo = DefaultCfg(); lo.dedup_similarity_threshold = -0.1;
    Status s = DataCleaner(lo).ValidateConfig();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_CLEANING_DEDUP_THRESHOLD_RANGE"), std::string::npos);
}

TEST(DataCleanerConfigTest, MaxChunkCharsNonPositive) {
    CleaningConfig cfg = DefaultCfg(); cfg.max_chunk_chars = 0;
    Status s = DataCleaner(cfg).ValidateConfig();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_CLEANING_ANOMALY_CONFIG_INVALID"), std::string::npos);
}

// ---------- Summarize (A-class meta) ----------

TEST(DataCleanerSummarizeTest, FourFlatFields) {
    DedupResult d; d.removed_count = 3;
    AnomalyResult a; a.total_marked = 1;
    CleaningResult r = DataCleaner::Summarize(/*input=*/12, d, a);
    EXPECT_EQ(r.chunks_input, 12);
    EXPECT_EQ(r.chunks_skipped_dedup, 3);
    EXPECT_EQ(r.chunks_indexed, 9);          // 12 - 3
    EXPECT_EQ(r.chunks_marked_anomalous, 1);
    // The exposed meta is exactly the 4 flat A-class keys (no cleaning_applied).
    nlohmann::json meta = r.ToMetaJson();
    EXPECT_EQ(meta.size(), 4u);
    EXPECT_TRUE(meta.contains("chunks_input"));
    EXPECT_TRUE(meta.contains("chunks_indexed"));
    EXPECT_TRUE(meta.contains("chunks_skipped_dedup"));
    EXPECT_TRUE(meta.contains("chunks_marked_anomalous"));
    EXPECT_FALSE(meta.contains("cleaning_applied"));
}

// ---------- NS config merge (reuse catalog ConfigResolver) ------

TEST(DataCleanerConfigMergeTest, NsOverridesGlobal) {
    cortrix::catalog::ConfigResolver<CleaningConfig> resolver;
    CleaningConfig global;  // defaults: dedup on, 0.95, anomaly on
    // ns_legal: stricter threshold.
    auto r = resolver.Resolve(global, R"({"dedup_similarity_threshold": 0.98})");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_DOUBLE_EQ(r.value().dedup_similarity_threshold, 0.98);
    EXPECT_TRUE(r.value().dedup_enabled);  // inherited
}

TEST(DataCleanerConfigMergeTest, NsPartialOverrideKeepsOthers) {
    cortrix::catalog::ConfigResolver<CleaningConfig> resolver;
    CleaningConfig global;
    auto r = resolver.Resolve(global, R"({"anomaly_detection_enabled": false})");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().anomaly_detection_enabled);
    EXPECT_DOUBLE_EQ(r.value().dedup_similarity_threshold, 0.95);  // unchanged
}

TEST(DataCleanerConfigMergeTest, EmptyNsInheritsGlobal) {
    cortrix::catalog::ConfigResolver<CleaningConfig> resolver;
    CleaningConfig global; global.dedup_similarity_threshold = 0.9;
    auto r = resolver.Resolve(global, "{}");
    ASSERT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(r.value().dedup_similarity_threshold, 0.9);
}

TEST(DataCleanerConfigMergeTest, InvalidNsJsonErrors) {
    cortrix::catalog::ConfigResolver<CleaningConfig> resolver;
    CleaningConfig global;
    auto r = resolver.Resolve(global, "not json");
    EXPECT_FALSE(r.ok());  // CX_ERR_INVALID_CONFIG_JSON (catalog domain)
}

}  // namespace
}  // namespace cortrix::spc
