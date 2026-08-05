// Error-path coverage for the Memory codes that had ZERO referencing
// test.
//
//  * CX_ERR_MEMORY_STORE       — MemoryBlockAdapter::ListByUser prepare failure
//                               (memory_block_adapter.cpp:185). Direct adapter test.
//  * CX_ERR_MEMEXTRACT_BLOCK_NOT_FOUND — the revoke route maps a NotFound from the
//                               extraction service to http 404 + this code
//                               (memory_routes.cpp:322-324). Wired-server test.
//
// MEMORYEXTRACT_REVOKE_FAILED is DEFERRED — see the note at the end of this file.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/memory/memory_block_adapter.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/memory/memory_extraction_service.h"
#include "cortrix/memory/memory_routes.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store_sqlite.h"

#include "mock_spc_manager.h"
#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using ::testing::_;

// ---------------------------------------------------------------------------
// CX_ERR_MEMORY_STORE — ListByUser prepare failure (ordinary input).
// ListByUser short-circuits to an empty result when db_handle() is null, so a no-op
// store does NOT reach the code. We need a REAL open handle with NO schema: open the
// store with run_schema_ddl=false so the `blocks` table is absent and the SELECT
// prepare fails ("no such table: blocks").
// ---------------------------------------------------------------------------
TEST(MemoryBlockAdapterErrPathTest, ListByUserMissingSchemaReturnsMemoryStore) {
    CortrixStoreSqlite::OpenOptions opts;
    opts.run_schema_ddl = false;     // open the handle but DO NOT create `blocks`
    opts.run_crash_recovery = false;
    CortrixStoreSqlite store(":memory:", opts);
    ASSERT_EQ(store.Open(), 0);
    ASSERT_NE(store.db_handle(), nullptr);  // handle open -> ListByUser will prepare

    memory::MemoryBlockAdapter adapter(store, /*embedder=*/nullptr, /*vec_index=*/nullptr);
    auto r = adapter.ListByUser("alice");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEMORY_STORE"), std::string::npos)
        << "message: " << r.status().message();
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
}

// ---------------------------------------------------------------------------
// CX_ERR_MEMEXTRACT_BLOCK_NOT_FOUND — revoke a never-seeded block on a wired server.
// The real MemoryExtractionService.RevokeInvalidation fetches the block, gets a
// NotFound (block id was never inserted), and the route maps http==404 to this code.
// ---------------------------------------------------------------------------
class MemoryRevokeNotFoundTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_errpath_mem_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        std::string key = "mem-errpath-key";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(key);
        kc.tenant_id = "test-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_.LoadKeys({kc});
        auth_key_ = key;

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(tmp_dir_ + "/pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();

        // Real extraction service (no LLM — revoke is a pure store transition).
        extraction_ = std::make_unique<memory::MemoryExtractionService>(
            harness_->ipool(), /*llm=*/nullptr, *embedder_, /*op_logger=*/nullptr,
            memory::MemoryExtractorConfig{}, memory::MemoryQueue::Config{});
        services_.extraction = extraction_.get();

        svr_ = std::make_unique<httplib::Server>();
        RegisterMemoryRoutes(*svr_, auth_, harness_->ipool(), mock_spc_, *embedder_,
                             *classifier_, *fusion_, config_.memory, services_);
        port_ = 19090 + ((getpid() + 13) % 900);
        svr_thread_ = std::thread([this] { svr_->listen("127.0.0.1", port_); });
        for (int i = 0; i < 50 && !svr_->is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        system(("rm -rf " + tmp_dir_).c_str());
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + auth_key_}};
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::string auth_key_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    cortrix::testing::MockSPCManager mock_spc_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<memory::MemoryExtractionService> extraction_;
    cortrix::MemoryServices services_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    int port_ = 0;
    std::string tmp_dir_;
};

TEST_F(MemoryRevokeNotFoundTest, RevokeUnknownBlockReturnsBlockNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    // ns is required AND the block was never inserted -> GetMemoryBlock NotFound ->
    // route maps http 404 -> CX_ERR_MEMEXTRACT_BLOCK_NOT_FOUND.
    auto res = cli.Post("/api/v1/memory/invalidations/blk_never_inserted/revoke",
                        AuthHeaders(), R"({"ns":"default","reason":"x"})",
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_MEMEXTRACT_BLOCK_NOT_FOUND") << res->body;
}

// ===========================================================================
// DEFERRED — no hollow test written:
//
//   CX_ERR_MEMEXTRACT_REVOKE_FAILED  (memory_routes.cpp:324, the else/http!=404 branch)
//     Reached only when MemoryExtractionService::RevokeInvalidation returns a non-ok
//     Status whose code() != kNotFound (i.e. a store UPDATE failure AFTER a successful
//     block fetch). MemoryExtractionService is a concrete, copy-deleted, NON-virtual
//     class with no interface seam, so a fake returning Status::Internal cannot be
//     substituted at the route layer; and the live revoke runs through the namespace pool
//     façade store, whose SetFailNextOps op-fault seam is not reachable from the route
//     fixture. A direct-adapter test could exercise the underlying
//     CX_ERR_MEMEXTRACT_STORE UPDATE path, but that is a DIFFERENT code than the route's
//     CX_ERR_MEMEXTRACT_REVOKE_FAILED mapping, which has no ordinary-input path. Triggering
//     it would need either a virtual seam on MemoryExtractionService or a façade store
//     exposing SetFailNextOps for the revoke connection.
// ===========================================================================

}  // namespace
}  // namespace cortrix
