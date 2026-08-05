// S3.1 — reranker.max_seq_length GUC (default 512 / range 128-8192) + the
// spc.chunk_size GUC registration backfilled (reranker V5-B2-05). Range-violation policy
// = REJECT (Status error), not clamp. Live PostgreSQL GUC registration = D3.5;
// these exercise the standalone reranker_guc.h SoT + IGlobalConfig load path.
#include <gtest/gtest.h>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/reranker/reranker_config.h"
#include "cortrix/reranker/reranker_guc.h"

namespace cortrix::reranker {
namespace {

// --- reranker.max_seq_length: default / boundaries / out-of-range reject ---

TEST(RerankerMaxSeqLenTest, DefaultIs512) {
    RerankerConfig c;
    EXPECT_EQ(c.max_seq_length, 512);
    EXPECT_TRUE(RerankerGuc::ValidateConfig(c).ok());
}

TEST(RerankerMaxSeqLenTest, KeyAndRangeConstantsMatchSpec) {
    // §2.4: reranker.max_seq_length, range 128-8192.
    EXPECT_STREQ(guc::kMaxSeqLength, "reranker.max_seq_length");
    EXPECT_EQ(guc::kMaxSeqLenMin, 128);
    EXPECT_EQ(guc::kMaxSeqLenMax, 8192);
}

TEST(RerankerMaxSeqLenTest, BoundaryValuesAccepted) {
    RerankerConfig c;
    c.max_seq_length = guc::kMaxSeqLenMin;  // 128
    EXPECT_TRUE(RerankerGuc::ValidateConfig(c).ok());
    c.max_seq_length = guc::kMaxSeqLenMax;  // 8192
    EXPECT_TRUE(RerankerGuc::ValidateConfig(c).ok());
}

TEST(RerankerMaxSeqLenTest, BelowMinRejected) {
    RerankerConfig c;
    c.max_seq_length = 127;
    Status s = RerankerGuc::ValidateConfig(c);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("reranker.max_seq_length"), std::string::npos);
    EXPECT_NE(s.message().find("127"), std::string::npos);
}

TEST(RerankerMaxSeqLenTest, AboveMaxRejected) {
    RerankerConfig c;
    c.max_seq_length = 8193;
    EXPECT_FALSE(RerankerGuc::ValidateConfig(c).ok());
}

TEST(RerankerMaxSeqLenTest, LoadFromGlobalConfigAppliesValueAndValidates) {
    InMemoryGlobalConfig cfg;
    cfg.Set("reranker.max_seq_length", "1024");
    RerankerConfig out;
    ASSERT_TRUE(RerankerGuc::LoadFromGlobalConfig(cfg, &out).ok());
    EXPECT_EQ(out.max_seq_length, 1024);
}

TEST(RerankerMaxSeqLenTest, LoadRejectsOutOfRangeMaxSeqLen) {
    InMemoryGlobalConfig cfg;
    cfg.Set("reranker.max_seq_length", "16384");  // > 8192
    RerankerConfig out;
    Status s = RerankerGuc::LoadFromGlobalConfig(cfg, &out);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("reranker.max_seq_length"), std::string::npos);
}

// --- spc.chunk_size GUC (read here for the §3.5.1 compat check) ---

TEST(SpcChunkSizeGucTest, AbsentDefaultsTo512) {
    InMemoryGlobalConfig cfg;  // no spc.chunk_size set
    Result<int> r = RerankerGuc::LoadSpcChunkSize(cfg);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), guc::kSpcChunkSizeDefault);
    EXPECT_EQ(r.value(), 512);
}

TEST(SpcChunkSizeGucTest, PresentValueRead) {
    InMemoryGlobalConfig cfg;
    cfg.Set("spc.chunk_size", "256");
    Result<int> r = RerankerGuc::LoadSpcChunkSize(cfg);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 256);
}

TEST(SpcChunkSizeGucTest, BoundaryValuesAccepted) {
    InMemoryGlobalConfig cfg;
    cfg.Set("spc.chunk_size", "128");
    EXPECT_TRUE(RerankerGuc::LoadSpcChunkSize(cfg).ok());
    cfg.Set("spc.chunk_size", "8192");
    EXPECT_TRUE(RerankerGuc::LoadSpcChunkSize(cfg).ok());
}

TEST(SpcChunkSizeGucTest, OutOfRangeRejected) {
    InMemoryGlobalConfig cfg;
    cfg.Set("spc.chunk_size", "100");  // < 128
    Status s = RerankerGuc::LoadSpcChunkSize(cfg).status();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("spc.chunk_size"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::reranker
