/// @file test_namespace_crossing.cpp
/// @brief Security tests: cross-namespace data leakage prevention
///
/// Validates that namespace isolation is enforced: data stored in one
/// namespace cannot be accessed, searched, or modified from another namespace.

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>

#include "httplib.h"
#include <nlohmann/json.hpp>

// [wire⑤c follow-up] MVP CortrixNamespace(Manager) + hnswlib headers removed;
// store-level isolation walks per-NS stores through the F05 pool façade and
// the HTTP fixture stands on the shared NsPoolHarness.
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/config/config.h"
#include "cortrix/resource/namespace_facade.h"
#include "unit/ns_pool_test_helper.h"
#include "cortrix/memory/memory_routes.h"
#include "cortrix/query/query_routes.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/spc_manager.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// Direct Store-Level Namespace Isolation
// ============================================================

class NamespaceCrossingStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_sec_nscross_" + std::to_string(getpid()));
        fs::create_directories(test_dir_);

        config_.ns.data_dir = test_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;

        harness_ = std::make_unique<test::NsPoolHarness>(test_dir_);
        ASSERT_TRUE(harness_->Admit("ns_alpha").ok());
        ASSERT_TRUE(harness_->Admit("ns_beta").ok());
    }

    void TearDown() override {
        harness_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    // Per-NS store via an acquired façade (D-I5a pattern); the façade member
    // keeps the bundle alive for the duration of the test body.
    CortrixStoreSqlite* AcquireStore(const std::string& ns,
                                     std::unique_ptr<resource::NamespaceFacade>& holder) {
        holder = std::make_unique<resource::NamespaceFacade>(harness_->ipool(), ns);
        if (!holder->Acquire().ok()) return nullptr;
        return dynamic_cast<CortrixStoreSqlite*>(&holder->store());
    }

    CortrixConfig config_;
    fs::path test_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
};

// SEC-NS-001: Documents in ns_alpha are invisible from ns_beta's store
TEST_F(NamespaceCrossingStoreTest, DocIsolationBetweenNamespaces) {
    // Insert doc in ns_alpha
    std::unique_ptr<resource::NamespaceFacade> fa, fb;
    CortrixStoreSqlite* store_a = AcquireStore("ns_alpha", fa);
    ASSERT_NE(store_a, nullptr);

    CortrixDoc doc;
    doc.source_type = "confidential";
    doc.source_path = "secret_alpha.txt";
    doc.content_hash = "alpha_secret_hash";
    ASSERT_EQ(store_a->doc_create(doc), 0);
    std::string alpha_doc_id = doc.doc_id;  // D-I6: ULID string

    // Access ns_beta's store
    CortrixStoreSqlite* store_b = AcquireStore("ns_beta", fb);
    ASSERT_NE(store_b, nullptr);

    // ns_beta should NOT see ns_alpha's doc
    CortrixDoc not_found;
    int ret = store_b->doc_get(alpha_doc_id, not_found);
    EXPECT_EQ(ret, -2) << "ns_beta should NOT find ns_alpha's doc by ID";

    CortrixDoc by_source;
    ret = store_b->doc_find_by_source("confidential", "secret_alpha.txt", by_source);
    EXPECT_EQ(ret, -2) << "ns_beta should NOT find ns_alpha's doc by source";

    // ns_beta should have zero docs
    int64_t count = 0;
    store_b->doc_count(&count);
    EXPECT_EQ(count, 0);
}

// SEC-NS-002: FTS5 search in ns_alpha doesn't leak into ns_beta
TEST_F(NamespaceCrossingStoreTest, FtsSearchIsolation) {
    std::unique_ptr<resource::NamespaceFacade> fa, fb;
    CortrixStoreSqlite* store_a = AcquireStore("ns_alpha", fa);
    ASSERT_NE(store_a, nullptr);

    // Seed searchable content in ns_alpha
    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "alpha.txt";
    store_a->doc_create(doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "classified alpha intel", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = "classified alpha intel";
    store_a->block_insert(block);

    // Search from ns_beta
    CortrixStoreSqlite* store_b = AcquireStore("ns_beta", fb);
    ASSERT_NE(store_b, nullptr);

    std::vector<SearchResult> results;
    int ret = store_b->search_fulltext("classified alpha intel", 10, results);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(results.empty())
        << "ns_beta FTS search leaked ns_alpha content (" << results.size() << " results)";
}

// SEC-NS-003: Delete in ns_alpha doesn't affect ns_beta
TEST_F(NamespaceCrossingStoreTest, DeleteIsolation) {
    std::unique_ptr<resource::NamespaceFacade> fa, fb;
    CortrixStoreSqlite* store_a = AcquireStore("ns_alpha", fa);
    CortrixStoreSqlite* store_b = AcquireStore("ns_beta", fb);
    ASSERT_NE(store_a, nullptr);
    ASSERT_NE(store_b, nullptr);

    // Create docs in both namespaces
    CortrixDoc doc_a, doc_b;
    doc_a.source_type = "test";
    doc_a.source_path = "alpha.txt";
    store_a->doc_create(doc_a);

    doc_b.source_type = "test";
    doc_b.source_path = "beta.txt";
    store_b->doc_create(doc_b);

    // Delete all docs in ns_alpha
    store_a->doc_delete(doc_a.doc_id);

    int64_t alpha_count = 0, beta_count = 0;
    store_a->doc_count(&alpha_count);
    store_b->doc_count(&beta_count);

    EXPECT_EQ(alpha_count, 0) << "ns_alpha should have 0 docs after delete";
    EXPECT_EQ(beta_count, 1) << "ns_beta should still have 1 doc";
}

// ============================================================
// HTTP-Level Namespace Isolation (Memory System)
// ============================================================

class NamespaceCrossingHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_sec_nscross_http_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        config_.ns.data_dir = tmp_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;
        config_.memory.inject_recent_turns = 3;
        config_.memory.inject_max_tokens = 2000;
        config_.memory.default_ttl_seconds = 0;
        config_.memory.max_sessions_per_namespace = 100;
        config_.memory.max_interactions_per_session = 500;

        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);
        ASSERT_TRUE(harness_->Admit("tenant_a").ok());
        ASSERT_TRUE(harness_->Admit("tenant_b").ok());

        auth_ = std::make_unique<ApiKeyAuth>();
        test_key_ = "sec-nscross-key";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key_);
        kc.tenant_id = "nscross-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({kc});

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();
        spc_mgr_ = std::make_unique<TestSPCManager>();

        svr_ = std::make_unique<httplib::Server>();
        RegisterMemoryRoutes(*svr_, *auth_, harness_->ipool(), *spc_mgr_,
                             *embedder_, *classifier_, *fusion_,
                             config_.memory);

        port_ = 19300 + (getpid() % 500);
        svr_thread_ = std::thread([this] { svr_->listen("127.0.0.1", port_); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Post("/api/v1/memory/sessions");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + test_key_}};
    }

    class TestSPCManager : public SPCManager {
    public:
        TestSPCManager() : SPCManager() {}
        Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
        int CancelBySourcePath(const std::string&) override { return 0; }
        void Start() override {}
        void Stop() override {}
        size_t QueueSize() const override { return 0; }
        SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    };

    CortrixConfig config_;
    fs::path tmp_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<TestSPCManager> spc_mgr_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string test_key_;
    int port_;
};

// SEC-NS-004: Memory search is scoped to correct namespace
TEST_F(NamespaceCrossingHttpTest, MemorySearchScopedToNamespace) {
    httplib::Client cli("127.0.0.1", port_);

    // Create session with interaction in tenant_a
    json sess_body;
    sess_body["namespace"] = "tenant_a";
    sess_body["user_id"] = "alice";
    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(),
                        sess_body.dump(), "application/json");
    ASSERT_TRUE(res && res->status == 201);
    std::string sid = json::parse(res->body)["session_id"].get<std::string>();

    // Write secret interaction
    json write_body;
    write_body["namespace"] = "tenant_a";
    write_body["query_text"] = "Top secret project Omega details";
    write_body["response_text"] = "Project Omega is classified";
    auto wr = cli.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                       AuthHeaders(), write_body.dump(), "application/json");
    ASSERT_TRUE(wr && wr->status == 201);

    // Search from tenant_b — should NOT find tenant_a's data
    json search_body;
    search_body["namespace"] = "tenant_b";
    search_body["query"] = "project Omega";
    auto search_res = cli.Post("/api/v1/memory/search", AuthHeaders(),
                               search_body.dump(), "application/json");
    ASSERT_TRUE(search_res);

    if (search_res->status == 200) {
        auto search_json = json::parse(search_res->body);
        if (search_json.contains("results")) {
            for (const auto& r : search_json["results"]) {
                std::string content = r.value("content", "");
                EXPECT_EQ(content.find("Omega"), std::string::npos)
                    << "tenant_b search leaked tenant_a's 'Omega' data";
            }
        }
    }
    // 404 or empty results both indicate proper isolation

    // List sessions in tenant_b — should NOT see tenant_a's session
    auto list_res = cli.Get("/api/v1/memory/sessions?namespace=tenant_b",
                            AuthHeaders());
    ASSERT_TRUE(list_res && list_res->status == 200);
    auto list_body = json::parse(list_res->body);
    for (const auto& sess : list_body["sessions"]) {
        EXPECT_NE(sess["session_id"], sid)
            << "tenant_a session leaked into tenant_b's list";
    }
}

}  // namespace
}  // namespace cortrix
