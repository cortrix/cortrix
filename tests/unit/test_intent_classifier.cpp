#include <gtest/gtest.h>

#include <vector>

#include "cortrix/query/intent_classifier.h"
#include "cortrix/retrieval/classifier.h"

namespace cortrix {
namespace {

class IntentClassifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Empty LLM config -> keyword-only mode
        LlmConfig config;
        classifier_ = std::make_unique<IntentClassifier>(config);
    }

    std::unique_ptr<IntentClassifier> classifier_;
};

// --- LLM mode detection ---

TEST_F(IntentClassifierTest, NoLlmConfig_KeywordMode) {
    EXPECT_FALSE(classifier_->IsLlmEnabled());
}

TEST(IntentClassifierTest_LlmDetection, WithLlmConfig_LlmEnabled) {
    LlmConfig config;
    config.provider = "openai";
    config.api_key = "sk-test";
    config.model = "gpt-4";
    IntentClassifier classifier(config);
    EXPECT_TRUE(classifier.IsLlmEnabled());
}

TEST(IntentClassifierTest_LlmDetection, EmptyProvider_KeywordMode) {
    LlmConfig config;
    config.api_key = "sk-test";
    IntentClassifier classifier(config);
    EXPECT_FALSE(classifier.IsLlmEnabled());
}

TEST(IntentClassifierTest_LlmDetection, EmptyApiKey_KeywordMode) {
    LlmConfig config;
    config.provider = "openai";
    IntentClassifier classifier(config);
    EXPECT_FALSE(classifier.IsLlmEnabled());
}

// --- Keyword classification: SQL keywords ---

TEST_F(IntentClassifierTest, SqlKeyword_SELECT) {
    EXPECT_EQ(classifier_->Classify("SELECT * FROM users", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SqlKeyword_COUNT) {
    EXPECT_EQ(classifier_->Classify("COUNT the number of records", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SqlKeyword_WHERE) {
    EXPECT_EQ(classifier_->Classify("WHERE status is active", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SqlKeyword_GroupBy) {
    EXPECT_EQ(classifier_->Classify("GROUP BY department", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SqlKeyword_JOIN) {
    EXPECT_EQ(classifier_->Classify("JOIN tables on id", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SqlKeyword_CaseInsensitive) {
    EXPECT_EQ(classifier_->Classify("select * from table", 1000000),
              QueryIntent::kSql);
}

// --- Keyword classification: Chinese SQL keywords ---

TEST_F(IntentClassifierTest, ChineseKeyword_Table) {
    EXPECT_EQ(classifier_->Classify("查看用户表", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, ChineseKeyword_Statistics) {
    EXPECT_EQ(classifier_->Classify("统计今年的销售额", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, ChineseKeyword_Query) {
    EXPECT_EQ(classifier_->Classify("查询最近的订单", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, ChineseKeyword_HowMany) {
    EXPECT_EQ(classifier_->Classify("有多少用户注册了", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, ChineseKeyword_Summary) {
    EXPECT_EQ(classifier_->Classify("汇总所有数据", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, ChineseKeyword_Ranking) {
    EXPECT_EQ(classifier_->Classify("排名前十的产品", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, ChineseKeyword_Count) {
    EXPECT_EQ(classifier_->Classify("几个项目在进行中", 1000000),
              QueryIntent::kSql);
}

// --- Keyword classification: semantic queries ---

TEST_F(IntentClassifierTest, SemanticQuery_Simple) {
    EXPECT_EQ(classifier_->Classify("find documents about risk management", 1000000),
              QueryIntent::kSemantic);
}

TEST_F(IntentClassifierTest, SemanticQuery_Question) {
    EXPECT_EQ(classifier_->Classify("what is the policy on data retention?", 1000000),
              QueryIntent::kSemantic);
}

TEST_F(IntentClassifierTest, SemanticQuery_Chinese) {
    EXPECT_EQ(classifier_->Classify("关于合同风险的文档", 1000000),
              QueryIntent::kSemantic);
}

TEST_F(IntentClassifierTest, SemanticQuery_Short) {
    EXPECT_EQ(classifier_->Classify("hello", 1000000),
              QueryIntent::kSemantic);
}

// --- Keyword fallback robustness tests ---

TEST_F(IntentClassifierTest, SqlKeyword_MixedCase_SUM) {
    // "SUM" should match (all uppercase in the keyword list)
    EXPECT_EQ(classifier_->Classify("get the SUM of revenue", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SqlKeyword_AVG) {
    EXPECT_EQ(classifier_->Classify("compute AVG salary", 1000000),
              QueryIntent::kSql);
}

// Design spec: INSERT/UPDATE/DELETE are NOT SQL keywords for intent classification.
// Text-to-SQL only generates SELECT statements; DML should route to semantic search.

TEST_F(IntentClassifierTest, DmlKeyword_DELETE_NotSql) {
    EXPECT_EQ(classifier_->Classify("DELETE old records", 1000000),
              QueryIntent::kSemantic);
}

TEST_F(IntentClassifierTest, DmlKeyword_INSERT_NotSql) {
    EXPECT_EQ(classifier_->Classify("INSERT new entry", 1000000),
              QueryIntent::kSemantic);
}

TEST_F(IntentClassifierTest, DmlKeyword_UPDATE_NotSql) {
    EXPECT_EQ(classifier_->Classify("UPDATE the status", 1000000),
              QueryIntent::kSemantic);
}

// New SQL keywords added per design: HAVING, LIMIT, MAX, MIN

TEST_F(IntentClassifierTest, SqlKeyword_HAVING) {
    EXPECT_EQ(classifier_->Classify("HAVING count > 5", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SqlKeyword_LIMIT) {
    EXPECT_EQ(classifier_->Classify("LIMIT 100 results", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SqlKeyword_MAX) {
    EXPECT_EQ(classifier_->Classify("find MAX salary", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SqlKeyword_MIN) {
    EXPECT_EQ(classifier_->Classify("find MIN price", 1000000),
              QueryIntent::kSql);
}

TEST_F(IntentClassifierTest, SemanticQuery_Empty) {
    // Empty string should return semantic (no SQL keywords found)
    EXPECT_EQ(classifier_->Classify("", 1000000),
              QueryIntent::kSemantic);
}

TEST_F(IntentClassifierTest, SemanticQuery_NoKeywords) {
    EXPECT_EQ(classifier_->Classify("tell me about cloud computing trends", 1000000),
              QueryIntent::kSemantic);
}

TEST_F(IntentClassifierTest, LlmFallback_WithLlmConfig) {
    // Even with LLM config, the MVP stub falls back to keyword classification
    LlmConfig config;
    config.provider = "openai";
    config.api_key = "sk-test";
    config.model = "gpt-4";
    IntentClassifier classifier(config);
    EXPECT_TRUE(classifier.IsLlmEnabled());

    // Should still work via keyword fallback (LLM stub calls ClassifyByKeyword)
    EXPECT_EQ(classifier.Classify("SELECT * FROM users", 1000000),
              QueryIntent::kSql);
    EXPECT_EQ(classifier.Classify("find risk documents", 1000000),
              QueryIntent::kSemantic);
}

// --- Coverage gap: ORDER BY keyword (two words) ---

TEST_F(IntentClassifierTest, SqlKeyword_ORDER_BY) {
    EXPECT_EQ(classifier_->Classify("ORDER BY name", 1000000),
              QueryIntent::kSql);
}

// --- Coverage gap: FROM keyword ---

TEST_F(IntentClassifierTest, SqlKeyword_FROM) {
    EXPECT_EQ(classifier_->Classify("data FROM the database", 1000000),
              QueryIntent::kSql);
}

// --- Coverage gap: LLM Classify path with timeout ---

TEST(IntentClassifierTest_LlmPath, LlmEnabled_ClassifyCalled) {
    LlmConfig config;
    config.provider = "openai";
    config.api_key = "sk-test";
    config.model = "gpt-4";
    IntentClassifier classifier(config);
    EXPECT_TRUE(classifier.IsLlmEnabled());

    // The LLM stub always calls ClassifyByKeyword, so SQL keywords still work
    EXPECT_EQ(classifier.Classify("COUNT all users", 1000000), QueryIntent::kSql);
    EXPECT_EQ(classifier.Classify("SUM of revenue", 1000000), QueryIntent::kSql);
    EXPECT_EQ(classifier.Classify("AVG salary by department", 1000000), QueryIntent::kSql);
}

// --- Coverage gap: Chinese keyword "ji ge" (how many) ---
// NOTE: the Chinese inputs in this file are intentional test vectors — they
// exercise the production Chinese SQL-keyword matching path in
// src/query/intent_classifier.cpp and MUST stay in Chinese.

TEST_F(IntentClassifierTest, ChineseKeyword_HowManyItems) {
    EXPECT_EQ(classifier_->Classify("几个项目", 1000000), QueryIntent::kSql);
}

// --- Coverage gap: LLM with timeout_us = 0 (edge case) ---

TEST(IntentClassifierTest_Edge, ZeroTimeout) {
    LlmConfig config;
    config.provider = "openai";
    config.api_key = "sk-test";
    IntentClassifier classifier(config);
    // Stub doesn't use timeout, should still work
    EXPECT_EQ(classifier.Classify("hello world", 0), QueryIntent::kSemantic);
}

// --- S2-D: IntentClassifier as a retrieval::IClassifier (backward-compatible) ---
// The shared-interface adapter is a pure addition; the existing 2-arg Classify and
// its call sites are unchanged. These tests cover the new IClassifier surface.

TEST(IntentClassifierTest_IClassifier, Identity) {
    LlmConfig config;  // keyword mode
    IntentClassifier classifier(config);
    EXPECT_EQ(classifier.Name(), "intent-classifier");
    EXPECT_EQ(classifier.Labels(),
              (std::vector<std::string>{"semantic", "sql", "hybrid"}));
    EXPECT_TRUE(classifier.IsAvailable());  // keyword matching is always available
}

TEST(IntentClassifierTest_IClassifier, IntentToLabelMapping) {
    EXPECT_STREQ(IntentClassifier::IntentToLabel(QueryIntent::kSemantic), "semantic");
    EXPECT_STREQ(IntentClassifier::IntentToLabel(QueryIntent::kSql), "sql");
    EXPECT_STREQ(IntentClassifier::IntentToLabel(QueryIntent::kHybrid), "hybrid");
}

TEST(IntentClassifierTest_IClassifier, ClassifyAdapterReturnsSqlLabel) {
    LlmConfig config;
    IntentClassifier classifier(config);
    retrieval::ClassificationResult r =
        classifier.Classify(retrieval::ClassifierInput{"SELECT * FROM users", {}, std::nullopt});
    EXPECT_EQ(r.label, "sql");
    EXPECT_FLOAT_EQ(r.confidence, 1.0f);
    EXPECT_FALSE(r.error_code.has_value());
}

TEST(IntentClassifierTest_IClassifier, ClassifyAdapterReturnsSemanticLabel) {
    LlmConfig config;
    IntentClassifier classifier(config);
    retrieval::ClassificationResult r = classifier.Classify(
        retrieval::ClassifierInput{"find documents about risk management", {}, std::nullopt});
    EXPECT_EQ(r.label, "semantic");
    EXPECT_FLOAT_EQ(r.score, 1.0f);
}

TEST(IntentClassifierTest_IClassifier, ExistingTwoArgApiStillWorks) {
    // Backward-compat guard: the legacy signature resolves and behaves as before
    // even though the class now also satisfies IClassifier.
    LlmConfig config;
    IntentClassifier classifier(config);
    EXPECT_EQ(classifier.Classify("COUNT the rows", 1000000), QueryIntent::kSql);
    EXPECT_EQ(classifier.Classify("tell me about clouds", 1000000), QueryIntent::kSemantic);
}

TEST(IntentClassifierTest_IClassifier, UsableThroughBasePointer) {
    LlmConfig config;
    IntentClassifier classifier(config);
    retrieval::IClassifier* base = &classifier;  // S1 polymorphic use
    auto r = base->Classify(retrieval::ClassifierInput{"GROUP BY dept", {}, std::nullopt});
    EXPECT_EQ(r.label, "sql");
    EXPECT_EQ(base->Name(), "intent-classifier");
    EXPECT_TRUE(base->IsAvailable());
}

}  // namespace
}  // namespace cortrix
