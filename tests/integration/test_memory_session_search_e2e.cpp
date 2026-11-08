// Memory-session search E2E on the production assembly (FullStackE2E::BuildIngest:
// real SPCManager + SPCPipeline + PHnsw + per-NS store).
//
// An interaction appended to a session is written by the memory-session fast path
// of SPCPipeline as one flat memory block. The memory searcher's scope filter reads
// the user_id from the blocks ROW metadata (PostFilter), not from the block's BLOB
// payload, so a row written without metadata is never returned by
// POST /api/v1/memory/search. test_e2e_full_server's MemorySearchEndpoint only checks
// for a 200 on a stub SPC layer that never writes blocks, which is why this went
// unnoticed; these tests check what the search returns.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/memory/memory_config.h"
#include "cortrix/memory/memory_routes.h"

#include "full_stack_e2e_harness.h"

namespace {

using json = nlohmann::json;

constexpr const char* kNs = "mem_session_search";
constexpr const char* kAnswerMarker = "PostgreSQL";

class MemorySessionSearchE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    h_ = std::make_unique<cortrix::test::FullStackE2E>();
    h_->BuildIngest(/*embedding_dim=*/128);
    // Owned by the tenant of user_key() ("alice"), so alice's requests pass the
    // namespace check on the routes that have one.
    ASSERT_TRUE(h_->CreateNamespaceOwnedBy(kNs, "alice").ok());
    cortrix::RegisterMemoryRoutes(h_->server(), h_->auth(), h_->pool(), h_->spc_mgr(),
                                  h_->embedder(), h_->classifier(), h_->fusion(),
                                  config_);
    h_->Start();
  }

  // Create a session owned by alice and append one interaction to it; returns the
  // session id (asserts both writes succeed).
  std::string WriteAliceInteraction() {
    auto c = h_->Client();
    json sess = {{"namespace", kNs}, {"user_id", "alice"}};
    auto sr = c.Post("/api/v1/memory/sessions", h_->Bearer(h_->user_key()), sess.dump(),
                     "application/json");
    EXPECT_TRUE(sr && sr->status == 201) << (sr ? sr->body : "no response");
    if (!sr || sr->status != 201) return "";
    const std::string sid = json::parse(sr->body)["session_id"].get<std::string>();

    json inter = {{"namespace", kNs},
                  {"user_id", "alice"},
                  {"query_text", "Which database does the billing service use?"},
                  {"response_text", "The billing service stores invoices in PostgreSQL."}};
    auto wr = c.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                     h_->Bearer(h_->user_key()), inter.dump(), "application/json");
    EXPECT_TRUE(wr && wr->status == 201) << (wr ? wr->body : "no response");
    return sid;
  }

  // POST /api/v1/memory/search as `key` for `user_id`; returns the results array.
  json Search(const std::string& key, const std::string& user_id) {
    auto c = h_->Client();
    json body = {{"namespace", kNs},
                 {"user_id", user_id},
                 {"query", "billing service database"}};
    auto res = c.Post("/api/v1/memory/search", h_->Bearer(key), body.dump(),
                      "application/json");
    EXPECT_TRUE(res && res->status == 200) << (res ? res->body : "no response");
    if (!res || res->status != 200) return json::array();
    return json::parse(res->body)["results"];
  }

  static bool MentionsAnswer(const json& results) {
    for (const auto& r : results) {
      const std::string text = r.value("query_text", "") + " " +
                               r.value("response_text", "") + " " + r.value("content", "");
      if (text.find(kAnswerMarker) != std::string::npos) return true;
    }
    return false;
  }

  // The interaction is processed asynchronously by the SPCManager worker, so poll the
  // search until it returns the interaction or the timeout passes.
  bool WaitUntilAliceFindsIt(int timeout_ms = 10000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (MentionsAnswer(Search(h_->user_key(), "alice"))) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }

  cortrix::MemoryConfig config_;
  std::unique_ptr<cortrix::test::FullStackE2E> h_;
};

TEST_F(MemorySessionSearchE2E, OwnerFindsInteractionWrittenToSession) {
  ASSERT_FALSE(WriteAliceInteraction().empty());
  EXPECT_TRUE(WaitUntilAliceFindsIt())
      << "an interaction written to alice's session was never returned by "
         "/api/v1/memory/search for alice";
}

TEST_F(MemorySessionSearchE2E, OtherUserDoesNotFindIt) {
  ASSERT_FALSE(WriteAliceInteraction().empty());
  // Wait until the interaction is searchable at all, so the negative check below is
  // not passing merely because processing has not finished.
  ASSERT_TRUE(WaitUntilAliceFindsIt());
  EXPECT_FALSE(MentionsAnswer(Search(h_->other_key(), "mallory")));
}

}  // namespace
