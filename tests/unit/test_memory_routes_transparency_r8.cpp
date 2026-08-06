// R8 memory_routes — memory transparency + opt-out SUCCESS-body coverage.
//
// Companion to test_memory_routes_success_r8.cpp (which covers the memory extract
// surface). This file drives the *transparency* CRUD (GET/POST/PATCH/DELETE
// /api/v1/memory + GET /api/v1/memory/invalidations) and the *opt-out* surface
// (POST /api/v1/memory/session/{id}/opt-out[/revoke]) through their 200/201 SUCCESS
// bodies — the arms test_memory_routes.cpp leaves uncovered (it only exercised
// auth/validation/disabled guards). No extraction service is wired (transparency +
// opt-out don't need it); the fixture is the same real-server + real namespace pool harness,
// minus the memory extraction service.
//
// Strategy: self-contained CRUD over the real endpoints — POST creates a memory block
// in the per-NS façade, then GET/PATCH/DELETE operate on the returned id. No reaching
// into the store: the create endpoint is the seed. Auth uses an admin key (read+write
// +admin) so the memory isolation own-user / session-owner guards pass for the happy path.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/config/config.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/memory/memory_routes.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/spc/onnx_embedder.h"

#include "mock_spc_manager.h"
#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// Real httplib server + real namespace pool (NsPoolHarness), NO extraction service: the
// transparency + opt-out routes are registered by RegisterMemoryRoutes regardless of
// MemoryServices.extraction, and neither surface needs it.
class MemoryRoutesTransparencyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmp_dir_ = "/tmp/cortrix_test_memroutes_transp_" + std::to_string(getpid());
    system(("mkdir -p " + tmp_dir_).c_str());

    config_.auth.enabled = true;
    config_.ns.data_dir = tmp_dir_;

    const std::string test_key = "test-api-key-12345";
    ApiKeyConfig key_config;
    key_config.key_hash = ApiKeyAuth::HashKey(test_key);
    key_config.tenant_id = "test-tenant";  // -> AuthContext.user_id (MVP)
    key_config.permissions = 7;            // read + write + admin
    config_.auth.api_keys.push_back(key_config);
    auth_.LoadKeys(config_.auth.api_keys);

    harness_ = std::make_unique<cortrix::test::NsPoolHarness>(tmp_dir_ + "/pool");
    ASSERT_TRUE(harness_->Admit("default").ok());

    port_ = 19480 + (getpid() % 1000);

    embedder_ = std::make_unique<OnnxEmbedder>("", 128);
    embedder_->Init();
    LlmConfig llm_cfg;
    classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
    fusion_ = std::make_unique<RRFFusion>();

    svr_ = std::make_unique<httplib::Server>();
    // Default MemoryServices (extraction = null) — transparency + opt-out routes are
    // mounted unconditionally; only the extract routes gate on the service.
    RegisterMemoryRoutes(*svr_, auth_, harness_->ipool(), mock_spc_, *embedder_,
                         *classifier_, *fusion_, config_.memory);

    svr_thread_ = std::thread([this] { svr_->listen("127.0.0.1", port_); });
    httplib::Client cli("127.0.0.1", port_);
    for (int i = 0; i < 50; ++i) {
      auto res = cli.Post("/api/v1/memory/sessions", "{}", "application/json");
      if (res) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  void TearDown() override {
    svr_->stop();
    if (svr_thread_.joinable()) svr_thread_.join();
    system(("rm -rf " + tmp_dir_).c_str());
  }

  httplib::Headers AuthHeaders() {
    return {{"Authorization", "Bearer test-api-key-12345"}};
  }

  // Create a memory via POST /api/v1/memory (admin user_id = "test-tenant"); returns
  // its memory_id (asserts 201 + active).
  std::string CreateMemory(const std::string& content,
                           const std::string& memory_type = "fact") {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"ns", "default"}, {"content", content}, {"memory_type", memory_type}};
    auto res = cli.Post("/api/v1/memory", AuthHeaders(), body.dump(), "application/json");
    EXPECT_TRUE(res);
    EXPECT_EQ(res ? res->status : 0, 201) << (res ? res->body : "no response");
    if (!res || res->status != 201) return "";
    auto j = json::parse(res->body);
    EXPECT_EQ(j.value("status", ""), "active");
    return j.value("memory_id", "");
  }

  // Create a memory session (for the opt-out tests); returns its session_id.
  std::string CreateSession() {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"namespace", "default"}};
    auto res = cli.Post("/api/v1/memory/sessions", AuthHeaders(), body.dump(),
                        "application/json");
    EXPECT_TRUE(res);
    EXPECT_EQ(res ? res->status : 0, 201) << (res ? res->body : "no response");
    if (!res || res->status != 201) return "";
    return json::parse(res->body).value("session_id", "");
  }

  std::string tmp_dir_;
  CortrixConfig config_;
  ApiKeyAuth auth_;
  std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
  cortrix::testing::MockSPCManager mock_spc_;
  std::unique_ptr<OnnxEmbedder> embedder_;
  std::unique_ptr<IntentClassifier> classifier_;
  std::unique_ptr<RRFFusion> fusion_;
  std::unique_ptr<httplib::Server> svr_;
  std::thread svr_thread_;
  int port_ = 0;
};

// ── memory transparency CRUD success bodies ──────────────────────────────────────

// POST /api/v1/memory → 201 {memory_id, status:"active"} (the create success body).
TEST_F(MemoryRoutesTransparencyTest, CreateMemoryReturns201) {
  const std::string id = CreateMemory("user prefers metric units");
  EXPECT_FALSE(id.empty());
}

// GET /api/v1/memory?ns=&user_id= → 200 {memories:[{memory_id,content,memory_type,
// status}], total} listing the created block (the list success body).
TEST_F(MemoryRoutesTransparencyTest, ListMemoriesReturnsCreated) {
  const std::string id = CreateMemory("user lives in Berlin", "fact");
  ASSERT_FALSE(id.empty());

  httplib::Client cli("127.0.0.1", port_);
  auto res = cli.Get("/api/v1/memory?ns=default&user_id=test-tenant", AuthHeaders());
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  ASSERT_TRUE(j.contains("memories"));
  ASSERT_TRUE(j["memories"].is_array());
  EXPECT_GE(j.value("total", 0), 1);
  bool found = false;
  for (const auto& m : j["memories"]) {
    if (m.value("memory_id", "") == id) {
      found = true;
      EXPECT_EQ(m.value("content", ""), "user lives in Berlin");
      EXPECT_EQ(m.value("status", ""), "active");
    }
  }
  EXPECT_TRUE(found) << "created memory not in list: " << j.dump();
}

// GET /api/v1/memory?explain=true → list with the memory transparency provenance fields surfaced
// (the explain branch of the list handler).
TEST_F(MemoryRoutesTransparencyTest, ListMemoriesExplainMode) {
  ASSERT_FALSE(CreateMemory("user speaks German").empty());
  httplib::Client cli("127.0.0.1", port_);
  auto res = cli.Get("/api/v1/memory?ns=default&user_id=test-tenant&explain=true",
                     AuthHeaders());
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  EXPECT_TRUE(j.contains("memories"));
}

// PATCH /api/v1/memory/{id} → 200 {new_memory_id, invalidated_memory_id} (edit =
// insert-new + invalidate-old success body).
TEST_F(MemoryRoutesTransparencyTest, EditMemoryReturnsNewAndInvalidatedIds) {
  const std::string id = CreateMemory("user prefers tea");
  ASSERT_FALSE(id.empty());

  httplib::Client cli("127.0.0.1", port_);
  json body{{"ns", "default"}, {"content", "user prefers coffee"}};
  auto res = cli.Patch("/api/v1/memory/" + id, AuthHeaders(), body.dump(),
                       "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  EXPECT_EQ(j.value("invalidated_memory_id", ""), id);
  EXPECT_FALSE(j.value("new_memory_id", "").empty());
  EXPECT_NE(j.value("new_memory_id", ""), id);
}

// DELETE /api/v1/memory/{id}?ns= → 200 {block_id, status:"invalidated"} (soft-delete
// success body). The block then appears under status=invalidated.
TEST_F(MemoryRoutesTransparencyTest, DeleteMemorySoftDeletes) {
  const std::string id = CreateMemory("user dislikes spoilers");
  ASSERT_FALSE(id.empty());

  httplib::Client cli("127.0.0.1", port_);
  auto res = cli.Delete("/api/v1/memory/" + id + "?ns=default&user_id=test-tenant",
                        AuthHeaders());
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  EXPECT_EQ(j.value("block_id", ""), id);
  EXPECT_EQ(j.value("status", ""), "invalidated");

  // It now shows up when explicitly listing invalidated memories.
  auto inv = cli.Get(
      "/api/v1/memory?ns=default&user_id=test-tenant&status=invalidated", AuthHeaders());
  ASSERT_TRUE(inv);
  ASSERT_EQ(inv->status, 200) << inv->body;
  auto ij = json::parse(inv->body);
  bool found = false;
  for (const auto& m : ij["memories"]) {
    if (m.value("memory_id", "") == id) found = true;
  }
  EXPECT_TRUE(found) << "soft-deleted memory not in invalidated list: " << ij.dump();
}

// GET /api/v1/memory/invalidations → 200 audit list (/ audit success body).
TEST_F(MemoryRoutesTransparencyTest, InvalidationsAuditListReturns200) {
  httplib::Client cli("127.0.0.1", port_);
  auto res = cli.Get("/api/v1/memory/invalidations?ns=default", AuthHeaders());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200) << res->body;
}

// ── opt-out success bodies ───────────────────────────────────────────────────────

// POST /api/v1/memory/session/{id}/opt-out → 200 {session_id, opt_out_at,
// opted_out_by} (the opt-out state-transition success body).
TEST_F(MemoryRoutesTransparencyTest, SessionOptOutReturns200) {
  const std::string sid = CreateSession();
  ASSERT_FALSE(sid.empty());

  httplib::Client cli("127.0.0.1", port_);
  json body{{"ns", "default"}, {"reason", "user requested"}};
  auto res = cli.Post("/api/v1/memory/session/" + sid + "/opt-out", AuthHeaders(),
                      body.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  EXPECT_EQ(j.value("session_id", ""), sid);
  EXPECT_FALSE(j.value("opt_out_at", "").empty());
  EXPECT_FALSE(j.value("opted_out_by", "").empty());
}

// POST /api/v1/memory/session/{id}/opt-out/revoke → 200 {session_id, revoked_at}
// (admin revoke success body; the route is kPermAdmin — the admin test key passes).
TEST_F(MemoryRoutesTransparencyTest, SessionOptOutRevokeReturns200) {
  const std::string sid = CreateSession();
  ASSERT_FALSE(sid.empty());

  httplib::Client cli("127.0.0.1", port_);
  // Opt out first so there is something to revoke.
  json oo{{"ns", "default"}, {"reason", "user requested"}};
  auto opt = cli.Post("/api/v1/memory/session/" + sid + "/opt-out", AuthHeaders(),
                      oo.dump(), "application/json");
  ASSERT_TRUE(opt);
  ASSERT_EQ(opt->status, 200) << opt->body;

  json rv{{"ns", "default"}, {"reason", "admin override"}};
  auto res = cli.Post("/api/v1/memory/session/" + sid + "/opt-out/revoke", AuthHeaders(),
                      rv.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto j = json::parse(res->body);
  EXPECT_EQ(j.value("session_id", ""), sid);
  EXPECT_FALSE(j.value("revoked_at", "").empty());
}

}  // namespace
}  // namespace cortrix
