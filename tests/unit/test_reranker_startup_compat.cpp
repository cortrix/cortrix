// S3.2 — startup compat check spc.chunk_size ≤ reranker.max_seq_length wired
// through IGlobalConfig (CX_ERR_CONFIG_MISMATCH on violation). The pure
// ValidateConfigCompat(int,int) is covered in test_reranker_init_failfast.cpp;
// this exercises the standalone GUC-load entry point (live startup wiring = D3.5).
#include <gtest/gtest.h>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/reranker/reranker_startup_validator.h"

namespace cortrix::reranker {
namespace {

TEST(RerankerStartupCompatTest, DefaultsPass) {
    // Both absent → spc.chunk_size=512, max_seq_length=512 → 512 ≤ 512 OK.
    InMemoryGlobalConfig cfg;
    EXPECT_TRUE(RerankerStartupValidator::ValidateStartupFromGlobalConfig(cfg).ok());
}

TEST(RerankerStartupCompatTest, SmallerChunkSizePasses) {
    InMemoryGlobalConfig cfg;
    cfg.Set("spc.chunk_size", "256");
    cfg.Set("reranker.max_seq_length", "512");
    EXPECT_TRUE(RerankerStartupValidator::ValidateStartupFromGlobalConfig(cfg).ok());
}

TEST(RerankerStartupCompatTest, RaisedMaxSeqLengthAdmitsLargerChunk) {
    // A business case: 1024-token chunks need reranker.max_seq_length raised too.
    InMemoryGlobalConfig cfg;
    cfg.Set("spc.chunk_size", "1024");
    cfg.Set("reranker.max_seq_length", "1024");
    EXPECT_TRUE(RerankerStartupValidator::ValidateStartupFromGlobalConfig(cfg).ok());
}

TEST(RerankerStartupCompatTest, ChunkSizeExceedsMaxSeqLengthFailsFast) {
    InMemoryGlobalConfig cfg;
    cfg.Set("spc.chunk_size", "1024");
    cfg.Set("reranker.max_seq_length", "512");
    Status s = RerankerStartupValidator::ValidateStartupFromGlobalConfig(cfg);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_CONFIG_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("1024"), std::string::npos);
    EXPECT_NE(s.message().find("512"), std::string::npos);
}

TEST(RerankerStartupCompatTest, OutOfRangeMaxSeqLengthRejectedBeforeCompat) {
    // An invalid reranker.max_seq_length surfaces as its own GUC error, not as a
    // compat mismatch.
    InMemoryGlobalConfig cfg;
    cfg.Set("reranker.max_seq_length", "70000");  // > 8192
    Status s = RerankerStartupValidator::ValidateStartupFromGlobalConfig(cfg);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("reranker.max_seq_length"), std::string::npos);
}

TEST(RerankerStartupCompatTest, OutOfRangeChunkSizeRejected) {
    InMemoryGlobalConfig cfg;
    cfg.Set("spc.chunk_size", "100");  // < 128
    Status s = RerankerStartupValidator::ValidateStartupFromGlobalConfig(cfg);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("spc.chunk_size"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::reranker
