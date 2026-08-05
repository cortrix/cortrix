// F41AsyncWorker — the F42 doc_summary task callback. These are
// integration-flavored unit tests: a real F05 DefaultNamespacePool (via the shared
// NsPoolHarness) + a real per-Unit store/F25 WriteCoordinator/FakeIndex, a real
// OnnxEmbedder("",128) stub, a real BlockAssembler, and a MockLlmClient driving the
// DocSummaryGenerator's structured output. The worker self-Acquires the task's
// namespace, so the fixture seeds/reads through short-lived scoped façades (one
// Acquire at a time) and lets the worker take its own.
#include "cortrix/doc_summary/f41_async_worker.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_type.h"
#include "cortrix/chunker/types.h"          // chunker::ChildChunk
#include "cortrix/common/block_types.h"     // kBlockDocSummary / kBlockFile
#include "cortrix/common/data_types.h"      // CortrixDoc / CortrixBlock
#include "cortrix/doc_summary/doc_summary_metrics.h"  // SummariesGeneratedCount (recorded by the generator)
#include "cortrix/id/hash.h"                // SetDeploymentHashKeyForTesting
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store.h"

#include "mock_llm_client.h"
#include "ns_pool_test_helper.h"

namespace cortrix::doc_summary {
namespace {

using ::testing::_;
using ::testing::NiceMock;

llm::ChatCompletionResponse OkChat(std::string content) {
    llm::ChatCompletionResponse r;
    r.content = std::move(content);
    r.model = "gpt-4o-mini";
    r.prompt_tokens = 100;
    r.completion_tokens = 60;
    return r;  // Status defaults ok()
}

// A valid §9.1 structured-output body the DocSummaryGenerator will parse.
const char* kSummaryJson = R"({
  "summary_text": "Q3 2026 financials: revenue grew twenty three percent year over year.",
  "keywords": ["revenue", "finance", "Q3"],
  "topics": ["Financial Report"],
  "one_liner": "Q3 2026 financials"
})";

class F41AsyncWorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Stable, non-zero block ids from HashChildIdToBlockId (assembler).
        id::SetDeploymentHashKeyForTesting({0x0123456789abcdefULL, 0xfedcba9876543210ULL});
        DocSummaryMetrics::Instance().ResetForTest();

        harness_ = std::make_unique<test::NsPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("f41_worker_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        ASSERT_TRUE(harness_->Admit("test-ns").ok());

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();

        ASSERT_TRUE(mgr_.Init(":memory:").ok());
    }

    void TearDown() override { harness_.reset(); }

    // Create a doc + `n_chunks` child chunks on the per-Unit store via a scoped
    // façade (Acquired then Released here). Returns the doc's ULID.
    std::string SeedDoc(int n_chunks) {
        resource::NamespaceFacade f(harness_->ipool(), "test-ns");
        EXPECT_TRUE(f.Acquire().ok());
        CortrixDoc doc;
        doc.source_type = "test";
        doc.source_path = "/f41/doc.txt";
        EXPECT_EQ(f.store().doc_create(doc), 0);
        BlockAssembler assembler;
        for (int i = 0; i < n_chunks; ++i) {
            chunker::ChildChunk c;
            c.child_id = "ch-" + doc.doc_id + "-" + std::to_string(i);
            c.parent_id = "par-" + doc.doc_id;
            c.doc_id = doc.doc_id;
            c.chunk_index = i;
            c.child_text = "chunk body number " + std::to_string(i);
            EmbeddingResult emb;  // child embedding irrelevant to the summary path
            CortrixBlock b = assembler.AssembleChild(c, emb, kBlockFile, 3, "");
            EXPECT_EQ(f.store().block_insert(b), 0);
        }
        return doc.doc_id;
    }

    // Read back every block of a doc through a scoped façade.
    std::vector<CortrixBlock> BlocksOf(const std::string& doc_id) {
        resource::NamespaceFacade f(harness_->ipool(), "test-ns");
        EXPECT_TRUE(f.Acquire().ok());
        std::vector<CortrixBlock> out;
        EXPECT_EQ(f.store().block_get_by_doc(doc_id, out), 0);
        return out;
    }

    // The single doc_summary block (block_type=17) of a doc, or nullopt.
    std::optional<CortrixBlock> DocSummaryBlockOf(const std::string& doc_id) {
        for (auto& b : BlocksOf(doc_id)) {
            if (b.block_type == static_cast<int>(kBlockDocSummary)) return b;
        }
        return std::nullopt;
    }

    std::shared_ptr<NiceMock<llm::MockLlmClient>> MakeLlm(const char* json) {
        auto m = std::make_shared<NiceMock<llm::MockLlmClient>>();
        ON_CALL(*m, Chat(_, _)).WillByDefault([json](const std::string&, const llm::LlmCallConfig&) {
            return OkChat(json);
        });
        return m;
    }

    F41AsyncWorker MakeWorker(std::shared_ptr<llm::ILlmClient> llm) {
        return F41AsyncWorker(harness_->ipool(), std::move(llm), DocSummaryConfig{},
                              *embedder_, assembler_, &mgr_);
    }

    async::TaskInfo SummaryTask(const std::string& doc_id, const std::string& ns = "test-ns") {
        async::TaskInfo t;
        t.doc_id = doc_id;
        t.namespace_id = ns;
        t.task_type = async::kTaskDocSummary;
        return t;
    }

    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    BlockAssembler assembler_;
    async::TaskManager mgr_;  // worker now self-finalizes; tests reaching ProcessTask
                              // directly leave it empty (MarkCompleted NotFound tolerated).
};

TEST_F(F41AsyncWorkerTest, WritesDocSummaryBlockOnSuccess) {
    const std::string doc_id = SeedDoc(3);
    F41AsyncWorker worker = MakeWorker(MakeLlm(kSummaryJson));

    Status s = worker.ProcessTask(SummaryTask(doc_id));
    ASSERT_TRUE(s.ok()) << s.message();

    auto block = DocSummaryBlockOf(doc_id);
    ASSERT_TRUE(block.has_value());
    EXPECT_NE(block->content_text.find("Q3 2026 financials"), std::string::npos);

    // metadata_json carries the §4.2 fields + status="generated".
    auto meta = nlohmann::json::parse(block->metadata_json, nullptr, false);
    ASSERT_FALSE(meta.is_discarded());
    EXPECT_EQ(meta.value("status", ""), "generated");
    EXPECT_EQ(meta["keywords"].size(), 3u);
    EXPECT_EQ(meta["topics"].size(), 1u);
    EXPECT_EQ(meta.value("one_liner", ""), "Q3 2026 financials");

    // The summary embedding went into the P-HNSW index (captured FakeIndex).
    EXPECT_FALSE(harness_->fake_index()->added_ids().empty());
    EXPECT_EQ(DocSummaryMetrics::Instance().SummariesGeneratedCount(), 1u);
}

TEST_F(F41AsyncWorkerTest, EmptyDocumentCompletesWithoutWritingBlock) {
    // No child chunks means an explicit empty document. The worker completes the
    // task without writing a synthetic doc_summary block.
    const std::string doc_id = SeedDoc(0);
    F41AsyncWorker worker = MakeWorker(MakeLlm(kSummaryJson));

    Status s = worker.ProcessTask(SummaryTask(doc_id));
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_FALSE(DocSummaryBlockOf(doc_id).has_value());
    EXPECT_EQ(DocSummaryMetrics::Instance().SummariesGeneratedCount(), 0u);
}

TEST_F(F41AsyncWorkerTest, InvalidLlmOutputWritesNoBlock) {
    const std::string doc_id = SeedDoc(2);
    F41AsyncWorker worker = MakeWorker(MakeLlm("not valid json"));

    Status s = worker.ProcessTask(SummaryTask(doc_id));
    EXPECT_FALSE(s.ok());
    EXPECT_FALSE(DocSummaryBlockOf(doc_id).has_value());
}

TEST_F(F41AsyncWorkerTest, AcquireFailureOnUnknownNamespaceReturnsError) {
    F41AsyncWorker worker = MakeWorker(MakeLlm(kSummaryJson));
    Status s = worker.ProcessTask(SummaryTask("some-doc", "no-such-ns"));
    EXPECT_FALSE(s.ok());
}

}  // namespace
}  // namespace cortrix::doc_summary
