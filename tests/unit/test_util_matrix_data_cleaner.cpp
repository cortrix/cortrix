// Exhaustive deterministic matrices for spc/data_cleaner.h (F10 DataCleaner) over
// Block-vector inputs: exact-dup / semantic near-dup / empty / unicode / anomaly
// reasons, plus ToString(AnomalyReason), Summarize, ValidateConfig, ShouldSkipIndex.
//
// Behavior (src/spc/data_cleaner.cpp):
//  - Dedup: SHA-256(chunk_text) exact pass keeps first occurrence; then semantic
//    cosine >= threshold drops later survivors. Disabled => no removals.
//  - Cosine of empty / mismatched-dim / zero-norm vectors == 0 (never "similar").
//  - DetectAnomaly marks (does NOT remove): EMPTY (len 0), OVERSIZED (>max_chars),
//    PARSE_FAILED (meta), INVALID_EMBEDDING (NaN/Inf or all-zero), DUPLICATE_BLOCK_ID
//    (non-empty id seen before). Sets flags_ext bit2 + metadata skip flags.
//  - oversized uses byte length (chunk_text.size()), not codepoints.
//
// SUITE PREFIX: DataCleanerUtilMatrix* (unique; DataCleanerDedupTest exists).
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "cortrix/common/block_types.h"
#include "cortrix/spc/data_cleaner.h"

namespace cortrix::spc {
namespace {

Block MakeBlock(const std::string& id, const std::string& text, int dim = 4,
                int axis = 0, float mag = 1.0f) {
    Block b;
    b.id = id;
    b.chunk_text = text;
    b.embedding.assign(dim, 0.0f);
    if (axis >= 0 && axis < dim) b.embedding[axis] = mag;
    return b;
}

CleaningConfig Cfg(bool dedup = true, double thr = 0.95, bool anomaly = true,
                   int max_chars = 10000) {
    CleaningConfig c;
    c.dedup_enabled = dedup;
    c.dedup_similarity_threshold = thr;
    c.anomaly_detection_enabled = anomaly;
    c.max_chunk_chars = max_chars;
    return c;
}

// ================= Exact-hash dedup matrix =================
TEST(DataCleanerUtilMatrixExactDedup, KeepsFirstOccurrence) {
    DataCleaner c(Cfg());
    // Distinct embedding axes so the semantic pass never fires (default axis=0
    // would make all blocks cosine-identical and also semantically deduped).
    std::vector<Block> blocks = {
        MakeBlock("a", "same", 4, 0), MakeBlock("b", "same", 4, 1),
        MakeBlock("c", "same", 4, 2), MakeBlock("d", "other", 4, 3)};
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.hash_dedup_count, 2);
    EXPECT_EQ(r.semantic_dedup_count, 0);
    EXPECT_EQ(r.removed_count, 2);
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].id, "a");
    EXPECT_EQ(blocks[1].id, "d");
    EXPECT_EQ(r.removed_block_ids, (std::vector<std::string>{"b", "c"}));
}

TEST(DataCleanerUtilMatrixExactDedup, EmptyTextDuplicatesAreExactDups) {
    DataCleaner c(Cfg());
    std::vector<Block> blocks = {MakeBlock("a", "", 4, 0), MakeBlock("b", "", 4, 1),
                                 MakeBlock("c", "x", 4, 2)};
    DedupResult r = c.Dedup(blocks);
    // Both empty strings hash identically -> second removed by exact pass. Distinct
    // embedding axes keep the semantic pass out of it.
    EXPECT_EQ(r.hash_dedup_count, 1);
    EXPECT_EQ(blocks.size(), 2u);
}

TEST(DataCleanerUtilMatrixExactDedup, UnicodeDistinctVsDuplicate) {
    DataCleaner c(Cfg());
    std::vector<Block> blocks = {
        MakeBlock("a", "\xE6\x96\x87\xE6\xA1\xA3", 4, 0),  // doc (CJK)
        MakeBlock("b", "\xE6\x96\x87\xE6\xA1\xA3", 4, 1),  // identical bytes
        MakeBlock("c", "\xE6\x95\xB0\xE6\x8D\xAE", 4, 2),  // different CJK
    };
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.hash_dedup_count, 1);
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].id, "a");
    EXPECT_EQ(blocks[1].id, "c");
}

TEST(DataCleanerUtilMatrixExactDedup, AllDistinctNothingRemoved) {
    DataCleaner c(Cfg());
    std::vector<Block> blocks = {MakeBlock("a", "one", 4, 0),
                                 MakeBlock("b", "two", 4, 1),
                                 MakeBlock("c", "three", 4, 2)};
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.removed_count, 0);
    EXPECT_EQ(r.kept_count, 3);
    EXPECT_EQ(blocks.size(), 3u);
}

// ================= Semantic dedup matrix =================
// Identical unit vectors (cosine 1.0) with distinct text -> semantic dedup.
TEST(DataCleanerUtilMatrixSemanticDedup, IdenticalVectorsDropped) {
    DataCleaner c(Cfg(true, 0.95));
    std::vector<Block> blocks = {
        MakeBlock("a", "text one", 4, 0, 1.0f),
        MakeBlock("b", "text two", 4, 0, 1.0f),  // same direction -> cos 1
        MakeBlock("c", "text three", 4, 1, 1.0f)  // orthogonal -> cos 0
    };
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.hash_dedup_count, 0);
    EXPECT_EQ(r.semantic_dedup_count, 1);
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].id, "a");
    EXPECT_EQ(blocks[1].id, "c");
}

// Orthogonal vectors (cosine 0) -> never deduped.
TEST(DataCleanerUtilMatrixSemanticDedup, OrthogonalVectorsKept) {
    DataCleaner c(Cfg(true, 0.95));
    std::vector<Block> blocks = {MakeBlock("a", "u", 4, 0, 1.0f),
                                 MakeBlock("b", "v", 4, 1, 1.0f),
                                 MakeBlock("c", "w", 4, 2, 1.0f)};
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.semantic_dedup_count, 0);
    EXPECT_EQ(blocks.size(), 3u);
}

// Threshold sensitivity: scaling magnitude does not change cosine (still 1.0).
TEST(DataCleanerUtilMatrixSemanticDedup, MagnitudeDoesNotAffectCosine) {
    DataCleaner c(Cfg(true, 0.99));
    std::vector<Block> blocks = {MakeBlock("a", "p", 4, 0, 1.0f),
                                 MakeBlock("b", "q", 4, 0, 7.5f)};
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.semantic_dedup_count, 1);
    EXPECT_EQ(blocks.size(), 1u);
}

// Threshold = 1.0 with strictly-less-than-1 cosine keeps both. Build two vectors
// with cosine < 1 (45-degree style) by setting two axes on one block.
TEST(DataCleanerUtilMatrixSemanticDedup, ThresholdOneKeepsNonIdentical) {
    DataCleaner c(Cfg(true, 1.0));
    Block a = MakeBlock("a", "p", 4, 0, 1.0f);  // (1,0,0,0)
    Block b = MakeBlock("b", "q", 4, 0, 1.0f);
    b.embedding = {1.0f, 1.0f, 0.0f, 0.0f};  // cosine = 1/sqrt(2) ~ 0.707 < 1.0
    std::vector<Block> blocks = {a, b};
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.semantic_dedup_count, 0);
    EXPECT_EQ(blocks.size(), 2u);
}

// Zero-norm and mismatched-dim vectors yield cosine 0 -> never semantic-deduped.
TEST(DataCleanerUtilMatrixSemanticDedup, ZeroAndMismatchedDimNotSimilar) {
    DataCleaner c(Cfg(true, 0.5));
    Block z1 = MakeBlock("z1", "aa", 4, -1, 0.0f);  // all zeros
    Block z2 = MakeBlock("z2", "bb", 4, -1, 0.0f);  // all zeros
    Block m = MakeBlock("m", "cc", 3, 0, 1.0f);     // different dim
    std::vector<Block> blocks = {z1, z2, m};
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.semantic_dedup_count, 0);  // cosine 0 for all pairs
    EXPECT_EQ(blocks.size(), 3u);
}

// Disabled dedup -> nothing removed even with exact dups.
TEST(DataCleanerUtilMatrixSemanticDedup, DisabledNoOp) {
    DataCleaner c(Cfg(/*dedup=*/false));
    std::vector<Block> blocks = {MakeBlock("a", "same"), MakeBlock("b", "same")};
    DedupResult r = c.Dedup(blocks);
    EXPECT_EQ(r.removed_count, 0);
    EXPECT_EQ(r.kept_count, 2);
    EXPECT_EQ(blocks.size(), 2u);
}

// ================= Anomaly detection matrix =================
struct AnomalyCase {
    std::string name;
    std::string text;
    int dim;
    int axis;    // -1 => all-zero embedding (invalid)
    float mag;
    int max_chars;
    bool use_nan;     // inject NaN into embedding[0]
    bool parse_fail;  // set meta.parse_status = failed
    bool expect_marked;
    std::string expect_reason_token;  // primary reason token to look for, "" = none
};

class DataCleanerUtilMatrixAnomaly
    : public ::testing::TestWithParam<AnomalyCase> {};

TEST_P(DataCleanerUtilMatrixAnomaly, Detect) {
    const AnomalyCase& p = GetParam();
    DataCleaner c(Cfg(true, 0.95, true, p.max_chars));
    Block b = MakeBlock("id1", p.text, p.dim, p.axis, p.mag);
    if (p.use_nan && !b.embedding.empty()) {
        b.embedding[0] = std::numeric_limits<float>::quiet_NaN();
    }
    if (p.parse_fail) b.metadata_json["meta.parse_status"] = "failed";
    std::vector<Block> blocks = {b};

    AnomalyResult r = c.DetectAnomaly(blocks);
    ASSERT_EQ(blocks.size(), 1u);  // never removed
    if (p.expect_marked) {
        EXPECT_EQ(r.total_marked, 1) << p.name;
        EXPECT_TRUE(blocks[0].flags_ext & kFlagExtIsAnomalous) << p.name;
        EXPECT_TRUE(ShouldSkipIndex(blocks[0])) << p.name;
        if (!p.expect_reason_token.empty()) {
            ASSERT_TRUE(blocks[0].metadata_json.contains("cleaning.anomaly_reason"));
            std::string reasons =
                blocks[0].metadata_json["cleaning.anomaly_reason"];
            EXPECT_NE(reasons.find(p.expect_reason_token), std::string::npos)
                << p.name << " reasons=" << reasons;
        }
    } else {
        EXPECT_EQ(r.total_marked, 0) << p.name;
        EXPECT_FALSE(blocks[0].flags_ext & kFlagExtIsAnomalous) << p.name;
        EXPECT_FALSE(ShouldSkipIndex(blocks[0])) << p.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Cases, DataCleanerUtilMatrixAnomaly,
    ::testing::Values(
        // Healthy block -> not marked.
        AnomalyCase{"healthy", "good text", 4, 0, 1.0f, 10000, false, false,
                    false, ""},
        // Empty chunk -> EMPTY + (all-zero embedding is also invalid). Primary
        // token we assert is "empty"; marked either way.
        AnomalyCase{"empty", "", 4, 0, 1.0f, 10000, false, false, true, "empty"},
        // Oversized: text length 6 > max_chars 5.
        AnomalyCase{"oversized", "abcdef", 4, 0, 1.0f, 5, false, false, true,
                    "oversized"},
        // Within size limit (len == max) -> not oversized.
        AnomalyCase{"atlimit", "abcde", 4, 0, 1.0f, 5, false, false, false, ""},
        // NaN embedding -> INVALID_EMBEDDING.
        AnomalyCase{"nan_embed", "txt", 4, 0, 1.0f, 10000, true, false, true,
                    "invalid_embedding"},
        // All-zero embedding -> INVALID_EMBEDDING.
        AnomalyCase{"zero_embed", "txt", 4, -1, 0.0f, 10000, false, false, true,
                    "invalid_embedding"},
        // Parse failure via metadata -> PARSE_FAILED.
        AnomalyCase{"parse_failed", "txt", 4, 0, 1.0f, 10000, false, true, true,
                    "parse_failed"},
        // Unicode oversized: 3 CJK chars = 9 bytes > max 5 bytes.
        AnomalyCase{"unicode_oversized",
                    "\xE6\x96\x87\xE6\xA1\xA3\xE6\x95\xB0", 4, 0, 1.0f, 5, false,
                    false, true, "oversized"}));

// Duplicate block ids -> second flagged DUPLICATE_BLOCK_ID.
TEST(DataCleanerUtilMatrixAnomalyDupId, SecondDuplicateIdMarked) {
    DataCleaner c(Cfg());
    std::vector<Block> blocks = {MakeBlock("dup", "a", 4, 0),
                                 MakeBlock("dup", "b", 4, 1),
                                 MakeBlock("uniq", "c", 4, 2)};
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.by_reason["duplicate_id"], 1);
    EXPECT_FALSE(blocks[0].flags_ext & kFlagExtIsAnomalous);
    EXPECT_TRUE(blocks[1].flags_ext & kFlagExtIsAnomalous);
    EXPECT_FALSE(blocks[2].flags_ext & kFlagExtIsAnomalous);
}

// Empty ids are not treated as duplicates of one another.
TEST(DataCleanerUtilMatrixAnomalyDupId, EmptyIdsNotDuplicates) {
    DataCleaner c(Cfg());
    std::vector<Block> blocks = {MakeBlock("", "a", 4, 0),
                                 MakeBlock("", "b", 4, 1)};
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.by_reason.count("duplicate_id"), 0u);
}

// Disabled anomaly detection -> no marks.
TEST(DataCleanerUtilMatrixAnomalyDupId, DisabledNoOp) {
    DataCleaner c(Cfg(true, 0.95, /*anomaly=*/false));
    std::vector<Block> blocks = {MakeBlock("a", "", 4, -1, 0.0f)};  // would be 2 reasons
    AnomalyResult r = c.DetectAnomaly(blocks);
    EXPECT_EQ(r.total_marked, 0);
    EXPECT_FALSE(blocks[0].flags_ext & kFlagExtIsAnomalous);
}

// ================= ToString(AnomalyReason) matrix =================
struct ReasonCase {
    AnomalyReason reason;
    std::string token;
};

class DataCleanerUtilMatrixReason : public ::testing::TestWithParam<ReasonCase> {};

TEST_P(DataCleanerUtilMatrixReason, Token) {
    const ReasonCase& c = GetParam();
    EXPECT_EQ(std::string(ToString(c.reason)), c.token);
}

INSTANTIATE_TEST_SUITE_P(
    Cases, DataCleanerUtilMatrixReason,
    ::testing::Values(
        ReasonCase{AnomalyReason::EMPTY_CHUNK, "empty"},
        ReasonCase{AnomalyReason::OVERSIZED_CHUNK, "oversized"},
        ReasonCase{AnomalyReason::PARSE_FAILED, "parse_failed"},
        ReasonCase{AnomalyReason::DUPLICATE_BLOCK_ID, "duplicate_id"},
        ReasonCase{AnomalyReason::INVALID_EMBEDDING, "invalid_embedding"}));

// ================= Summarize matrix =================
struct SummarizeCase {
    int input;
    int dedup_removed;
    int anomalous;
};

class DataCleanerUtilMatrixSummarize
    : public ::testing::TestWithParam<SummarizeCase> {};

TEST_P(DataCleanerUtilMatrixSummarize, Arithmetic) {
    const SummarizeCase& p = GetParam();
    DedupResult d;
    d.removed_count = p.dedup_removed;
    AnomalyResult a;
    a.total_marked = p.anomalous;
    CleaningResult r = DataCleaner::Summarize(p.input, d, a);
    EXPECT_EQ(r.chunks_input, p.input);
    EXPECT_EQ(r.chunks_skipped_dedup, p.dedup_removed);
    EXPECT_EQ(r.chunks_indexed, p.input - p.dedup_removed);
    EXPECT_EQ(r.chunks_marked_anomalous, p.anomalous);
}

INSTANTIATE_TEST_SUITE_P(
    Cases, DataCleanerUtilMatrixSummarize,
    ::testing::Values(SummarizeCase{0, 0, 0}, SummarizeCase{10, 0, 0},
                      SummarizeCase{10, 3, 2}, SummarizeCase{100, 50, 10},
                      SummarizeCase{5, 5, 0}, SummarizeCase{1, 0, 1},
                      SummarizeCase{1000, 1, 999}));

// ================= ValidateConfig matrix =================
struct ValidateCase {
    double threshold;
    int max_chars;
    bool ok;
};

class DataCleanerUtilMatrixValidate
    : public ::testing::TestWithParam<ValidateCase> {};

TEST_P(DataCleanerUtilMatrixValidate, Validate) {
    const ValidateCase& p = GetParam();
    DataCleaner c(Cfg(true, p.threshold, true, p.max_chars));
    Status s = c.ValidateConfig();
    EXPECT_EQ(s.ok(), p.ok)
        << "threshold=" << p.threshold << " max_chars=" << p.max_chars;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, DataCleanerUtilMatrixValidate,
    ::testing::Values(
        // Valid thresholds in [0,1].
        ValidateCase{0.0, 10000, true}, ValidateCase{0.5, 10000, true},
        ValidateCase{0.95, 100, true}, ValidateCase{1.0, 1, true},
        // Out-of-range threshold.
        ValidateCase{-0.01, 10000, false}, ValidateCase{1.01, 10000, false},
        ValidateCase{2.0, 10000, false}, ValidateCase{-1.0, 10000, false},
        // Invalid max_chars (<= 0).
        ValidateCase{0.5, 0, false}, ValidateCase{0.5, -1, false},
        ValidateCase{0.5, -100, false}));

// ================= ShouldSkipIndex matrix =================
TEST(DataCleanerUtilMatrixSkipIndex, FlagBitTriggersSkip) {
    Block b = MakeBlock("a", "x");
    EXPECT_FALSE(ShouldSkipIndex(b));
    b.flags_ext |= kFlagExtIsAnomalous;
    EXPECT_TRUE(ShouldSkipIndex(b));
}

TEST(DataCleanerUtilMatrixSkipIndex, MetadataBoolTrueTriggersSkip) {
    Block b = MakeBlock("a", "x");
    b.metadata_json["cleaning.skip_index"] = true;
    EXPECT_TRUE(ShouldSkipIndex(b));
}

TEST(DataCleanerUtilMatrixSkipIndex, MetadataBoolFalseDoesNotSkip) {
    Block b = MakeBlock("a", "x");
    b.metadata_json["cleaning.skip_index"] = false;
    EXPECT_FALSE(ShouldSkipIndex(b));
}

TEST(DataCleanerUtilMatrixSkipIndex, NonBoolSkipFieldIgnored) {
    Block b = MakeBlock("a", "x");
    b.metadata_json["cleaning.skip_index"] = "true";  // string, not bool
    EXPECT_FALSE(ShouldSkipIndex(b));
}

}  // namespace
}  // namespace cortrix::spc
