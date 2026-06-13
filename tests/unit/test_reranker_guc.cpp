// S2.2 — reranker.queue_size + reranker.task_timeout_ms GUC registration +
// range validation (50-2000 / 1000-30000). (max_seq_length range = S3.1.)
#include <gtest/gtest.h>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/reranker/reranker_config.h"
#include "cortrix/reranker/reranker_guc.h"

namespace cortrix::reranker {
namespace {

TEST(RerankerGucTest, DefaultConfigIsValid) {
    RerankerConfig c;  // struct defaults = §2.4 defaults
    EXPECT_TRUE(RerankerGuc::ValidateConfig(c).ok());
    EXPECT_EQ(c.queue_size, 200);
    EXPECT_EQ(c.task_timeout_ms, 5000);
}

TEST(RerankerGucTest, QueueSizeBoundaryValues) {
    RerankerConfig c;
    c.queue_size = guc::kQueueSizeMin;  // 50
    EXPECT_TRUE(RerankerGuc::ValidateConfig(c).ok());
    c.queue_size = guc::kQueueSizeMax;  // 2000
    EXPECT_TRUE(RerankerGuc::ValidateConfig(c).ok());
}

TEST(RerankerGucTest, QueueSizeBelowMinRejected) {
    RerankerConfig c;
    c.queue_size = 49;
    Status s = RerankerGuc::ValidateConfig(c);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("reranker.queue_size"), std::string::npos);
    EXPECT_NE(s.message().find("49"), std::string::npos);
}

TEST(RerankerGucTest, QueueSizeAboveMaxRejected) {
    RerankerConfig c;
    c.queue_size = 2001;
    EXPECT_FALSE(RerankerGuc::ValidateConfig(c).ok());
}

TEST(RerankerGucTest, TaskTimeoutBoundaryValues) {
    RerankerConfig c;
    c.task_timeout_ms = guc::kTaskTimeoutMin;  // 1000
    EXPECT_TRUE(RerankerGuc::ValidateConfig(c).ok());
    c.task_timeout_ms = guc::kTaskTimeoutMax;  // 30000
    EXPECT_TRUE(RerankerGuc::ValidateConfig(c).ok());
}

TEST(RerankerGucTest, TaskTimeoutOutOfRangeRejected) {
    RerankerConfig c;
    c.task_timeout_ms = 999;
    EXPECT_FALSE(RerankerGuc::ValidateConfig(c).ok());
    c.task_timeout_ms = 30001;
    Status s = RerankerGuc::ValidateConfig(c);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("reranker.task_timeout_ms"), std::string::npos);
}

TEST(RerankerGucTest, ValidateRangeNamesKeyAndBounds) {
    Status s = RerankerGuc::ValidateRange("reranker.queue_size", 5, 50, 2000);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("reranker.queue_size"), std::string::npos);
    EXPECT_NE(s.message().find("[50, 2000]"), std::string::npos);
}

// --- LoadFromGlobalConfig: present keys override defaults, absent keep default ---

TEST(RerankerGucTest, LoadAppliesPresentKeysAndKeepsDefaults) {
    InMemoryGlobalConfig cfg;
    cfg.Set("reranker.queue_size", "500");
    cfg.Set("reranker.task_timeout_ms", "8000");
    // workers / max_seq_length absent → keep struct defaults.

    RerankerConfig out;
    Status s = RerankerGuc::LoadFromGlobalConfig(cfg, &out);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(out.queue_size, 500);
    EXPECT_EQ(out.task_timeout_ms, 8000);
    EXPECT_EQ(out.workers, 4);            // default kept
    EXPECT_EQ(out.max_seq_length, 512);   // default kept
}

TEST(RerankerGucTest, LoadRejectsOutOfRangeKey) {
    InMemoryGlobalConfig cfg;
    cfg.Set("reranker.queue_size", "9999");  // > max 2000
    RerankerConfig out;
    Status s = RerankerGuc::LoadFromGlobalConfig(cfg, &out);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("reranker.queue_size"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::reranker
