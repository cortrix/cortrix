// Unit coverage for the new wiring logic introduced when
// mounting the CE auth surface (P1), DB import (P2), batch (P3), and the
// agent_llm config (P4). The route handlers + services are exercised by their
// own suites (test_api_keys / test_import_manager / test_batch_routes); here we
// cover the two NEW pieces of logic this wave added:
//   * ApiKeyAuth::SetApiKeyService DB-key fallback (P1)
//   * AgentLlmConfig codec round-trip + masking, via InMemoryGlobalConfig (P4)

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/common/agent_llm_config_codec.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/import/spc_manager_feeder.h"
#include "cortrix/import/import_types.h"
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/server/i_task_submitter.h"
#include "../mocks/mock_spc_manager.h"

namespace cortrix {
namespace {

// ---- P1: ApiKeyAuth DB-backed key fallback ---------------------------------

class ApiKeyAuthDbFallbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        // FK api_keys.user_id → users.id; seed an owner.
        const std::string sql =
            "INSERT INTO users(id,email,password_hash,display_name,email_verified,"
            "status,login_attempts,created_at,updated_at) VALUES('usr_p1',"
            "'p1@e.com','h','P1',1,'active',0,1,1)";
        ASSERT_EQ(sqlite3_exec(pdb_.db(), sql.c_str(), nullptr, nullptr, nullptr),
                  SQLITE_OK);
        svc_ = std::make_unique<auth::ApiKeyService>(pdb_.db());
        auth_.set_enabled(true);
        auth_.SetApiKeyService(svc_.get());
    }
    auth::PlatformDb pdb_;
    std::unique_ptr<auth::ApiKeyService> svc_;
    ApiKeyAuth auth_;
};

TEST_F(ApiKeyAuthDbFallbackTest, ValidDbKeyAuthenticatesAsAdmin) {
    auto created = svc_->CreateApiKey("usr_p1", "agent-key");
    ASSERT_TRUE(created.ok());

    AuthContext ctx;
    Status s = auth_.Authenticate(created.value().plaintext, &ctx);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(ctx.user_id, "usr_p1");
    EXPECT_TRUE(ctx.is_admin());
    EXPECT_TRUE(ctx.can_read());
    EXPECT_TRUE(ctx.can_write());
}

TEST_F(ApiKeyAuthDbFallbackTest, UnknownKeyStillRejected) {
    AuthContext ctx;
    Status s = auth_.Authenticate("cortrix_sk_not_a_real_key", &ctx);
    EXPECT_FALSE(s.ok());
}

TEST_F(ApiKeyAuthDbFallbackTest, RevokedDbKeyRejected) {
    auto created = svc_->CreateApiKey("usr_p1", "to-revoke");
    ASSERT_TRUE(created.ok());
    ASSERT_TRUE(svc_->RevokeApiKey(created.value().info.id).ok());

    AuthContext ctx;
    Status s = auth_.Authenticate(created.value().plaintext, &ctx);
    EXPECT_FALSE(s.ok());
}

TEST(ApiKeyAuthNoServiceTest, FallbackInactiveWithoutService) {
    // Without SetApiKeyService, an unknown token is rejected (config keys only).
    ApiKeyAuth auth;
    auth.set_enabled(true);
    AuthContext ctx;
    EXPECT_FALSE(auth.Authenticate("cortrix_sk_whatever", &ctx).ok());
}

// ---- P4: AgentLlmConfig codec round-trip + masking -------------------------

TEST(AgentLlmCodecTest, StoreLoadRoundTripThroughGlobalConfig) {
    InMemoryGlobalConfig cfg;
    AgentLlmConfig in;
    in.provider = "openai";
    in.api_key = "sk-secret-123456";
    in.model = "gpt-4o";
    in.base_url = "https://api.openai.com/v1";
    in.max_tokens = 2048;
    in.temperature = 0.3;
    cfg.SetAgentLlmConfig(in);

    AgentLlmConfig out = cfg.GetAgentLlmConfig();
    EXPECT_EQ(out.provider, "openai");
    EXPECT_EQ(out.api_key, "sk-secret-123456");  // decrypted back to plaintext
    EXPECT_EQ(out.model, "gpt-4o");
    EXPECT_EQ(out.base_url, "https://api.openai.com/v1");
    EXPECT_EQ(out.max_tokens, 2048);
    // temperature round-trips through GetFloat (single precision) — near, not exact.
    EXPECT_NEAR(out.temperature, 0.3, 1e-6);
}

TEST(AgentLlmCodecTest, ApiKeyStoredEncryptedNotPlaintext) {
    InMemoryGlobalConfig cfg;
    AgentLlmConfig in;
    in.provider = "claude";
    in.api_key = "sk-ant-PLAINTEXT-MARKER";
    cfg.SetAgentLlmConfig(in);

    // The raw stored value must NOT contain the plaintext key (encrypted at rest).
    auto raw = cfg.GetString("agent_llm.api_key_enc");
    ASSERT_TRUE(raw.ok());
    EXPECT_EQ(raw.value().find("PLAINTEXT-MARKER"), std::string::npos);
    EXPECT_FALSE(raw.value().empty());
}

TEST(AgentLlmCodecTest, DefaultsWhenUnconfigured) {
    InMemoryGlobalConfig cfg;
    AgentLlmConfig out = cfg.GetAgentLlmConfig();
    EXPECT_TRUE(out.provider.empty());     // unconfigured → Mock fallback
    EXPECT_TRUE(out.api_key.empty());
    EXPECT_EQ(out.max_tokens, 4096);       // struct defaults
    EXPECT_DOUBLE_EQ(out.temperature, 0.7);
}

TEST(AgentLlmCodecTest, MaskApiKey) {
    EXPECT_EQ(agent_llm_codec::MaskApiKey(""), "");
    EXPECT_EQ(agent_llm_codec::MaskApiKey("abc"), "...");
    EXPECT_EQ(agent_llm_codec::MaskApiKey("sk-1234567890"), "sk-1...");
}

TEST(AgentLlmCodecTest, EmptyApiKeyStoresEmptyBlob) {
    InMemoryGlobalConfig cfg;
    AgentLlmConfig in;
    in.provider = "ollama";
    in.api_key = "";  // ollama needs no key
    cfg.SetAgentLlmConfig(in);

    auto raw = cfg.GetString("agent_llm.api_key_enc");
    ASSERT_TRUE(raw.ok());
    EXPECT_TRUE(raw.value().empty());
    EXPECT_TRUE(cfg.GetAgentLlmConfig().api_key.empty());
}

// ---- P2b: DB import real SPCManager feeder ---------------------------------

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

TEST(SpcManagerFeederTest, FeedsEachChunkAsDocViaProcessParsedDoc) {
    testing::MockSPCManager spc;
    import::SpcManagerFeeder feeder(&spc);

    // Two textualized rows → two ProcessParsedDoc calls, both succeeding (rc=0).
    EXPECT_CALL(spc, ProcessParsedDoc(_, _)).Times(2).WillRepeatedly(Return(0));

    std::vector<import::TextChunk> chunks(2);
    chunks[0].text = "id: 1\nname: alice";
    chunks[0].source = "postgres://h:5432/db/users/1";
    chunks[0].metadata = {{"source_type", "db_import"}};
    chunks[1].text = "id: 2\nname: bob";
    chunks[1].source = "postgres://h:5432/db/users/2";

    auto r = feeder.Feed(chunks, "ns_demo");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), 2);
}

TEST(SpcManagerFeederTest, MapsChunkTextAndSourceIntoTask) {
    testing::MockSPCManager spc;
    import::SpcManagerFeeder feeder(&spc);

    // ParsedDoc / SPCTask are passed by mutable reference (SPCTask holds atomics →
    // not copyable), so extract the fields inside the mock action rather than
    // SaveArg-copying the whole struct.
    std::string ns, source_path, source_type, doc_id, content_hash, meta_json, page_text;
    size_t page_count = 0;
    EXPECT_CALL(spc, ProcessParsedDoc(_, _))
        .WillOnce(Invoke([&](spc::ParsedDoc& parsed, SPCTask& task) -> int {
            ns = task.namespace_name;
            source_path = task.source_path;
            source_type = task.source_type;
            doc_id = task.doc_id;
            content_hash = task.content_hash;
            meta_json = task.metadata_json;
            page_count = parsed.pages.size();
            if (!parsed.pages.empty()) page_text = parsed.pages[0].page_text;
            return 0;
        }));

    std::vector<import::TextChunk> chunks(1);
    chunks[0].text = "the row text";
    chunks[0].source = "postgres://h/db/orders/42";
    chunks[0].metadata = {{"ref", "db_conn_x"}};

    ASSERT_TRUE(feeder.Feed(chunks, "ns_orders").ok());
    EXPECT_EQ(ns, "ns_orders");
    EXPECT_EQ(source_path, "postgres://h/db/orders/42");
    EXPECT_EQ(source_type, "db_import");
    EXPECT_FALSE(doc_id.empty());            // a fresh ULID was minted
    EXPECT_FALSE(content_hash.empty());      // text-derived hash
    EXPECT_NE(meta_json.find("db_conn_x"), std::string::npos);
    ASSERT_EQ(page_count, 1u);                // one-page parsed doc
    EXPECT_EQ(page_text, "the row text");
}

TEST(SpcManagerFeederTest, PipelineFailureAbortsWithError) {
    testing::MockSPCManager spc;
    import::SpcManagerFeeder feeder(&spc);
    EXPECT_CALL(spc, ProcessParsedDoc(_, _)).WillOnce(Return(-1));  // pipeline error

    std::vector<import::TextChunk> chunks(1);
    chunks[0].text = "boom";
    chunks[0].source = "postgres://h/db/t/9";

    auto r = feeder.Feed(chunks, "ns");
    EXPECT_FALSE(r.ok());
}

TEST(NoopBlockCleanerTest, ReturnsZeroNeverFails) {
    import::NoopBlockCleaner cleaner;
    auto r = cleaner.CleanupSourceBlocks("ns", "postgres://h/db/users/");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 0);
}

// ---- P3b: batch inline content → temp-file materialization ------------------

namespace {
// A submitter that captures the SubmitRequest.filepath each doc was submitted with.
class CapturingSubmitter : public server::ITaskSubmitter {
public:
    Result<async::TaskInfo> Submit(const async::SubmitRequest& req) override {
        filepaths.push_back(req.filepath);
        async::TaskInfo info;
        info.task_id = "task_" + req.doc_id;
        return info;
    }
    std::vector<std::string> filepaths;
};
}  // namespace

TEST(BatchMaterializeTest, WritesContentAndSetsFilepathWhenEnabled) {
    CapturingSubmitter submitter;
    server::BatchSubmitService svc(&submitter);
    const std::string dir =
        (std::filesystem::temp_directory_path() / ("cortrix_batch_p3b_" +
         std::to_string(getpid()))).string();
    svc.SetMaterializeDir(dir);

    server::BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"doc-a", "hello content A", ""});
    req.documents.push_back({"doc-b", "hello content B", ""});

    auto res = svc.Submit(req);
    EXPECT_EQ(res.status, 200);
    ASSERT_EQ(submitter.filepaths.size(), 2u);

    // Each filepath is non-empty, exists, and holds the exact content.
    for (size_t i = 0; i < submitter.filepaths.size(); ++i) {
        const std::string& fp = submitter.filepaths[i];
        ASSERT_FALSE(fp.empty());
        ASSERT_TRUE(std::filesystem::exists(fp));
        std::ifstream in(fp, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        EXPECT_EQ(body, req.documents[i].content);
    }
    std::filesystem::remove_all(dir);
}

TEST(BatchMaterializeTest, EmptyFilepathWhenDisabled) {
    CapturingSubmitter submitter;
    server::BatchSubmitService svc(&submitter);  // no SetMaterializeDir → disabled

    server::BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"doc-a", "content", ""});

    auto res = svc.Submit(req);
    EXPECT_EQ(res.status, 200);
    ASSERT_EQ(submitter.filepaths.size(), 1u);
    EXPECT_TRUE(submitter.filepaths[0].empty());  // standalone/mock seam preserved
}

}  // namespace
}  // namespace cortrix
