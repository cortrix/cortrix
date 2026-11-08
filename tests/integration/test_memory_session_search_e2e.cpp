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
#include <sqlite3.h>

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
  // Create a session owned by alice; `sid` non-empty asks for that session id.
  std::string CreateAliceSession(const std::string& sid = "") {
    auto c = h_->Client();
    json sess = {{"namespace", kNs}, {"user_id", "alice"}};
    if (!sid.empty()) sess["session_id"] = sid;
    auto sr = c.Post("/api/v1/memory/sessions", h_->Bearer(h_->user_key()), sess.dump(),
                     "application/json");
    EXPECT_TRUE(sr && sr->status == 201) << (sr ? sr->body : "no response");
    if (!sr || sr->status != 201) return "";
    return json::parse(sr->body)["session_id"].get<std::string>();
  }

  // Append the billing interaction to `sid` as alice; `body_user_id` empty = omit the
  // field. Returns the HTTP status (0 on no response).
  int AppendInteraction(const std::string& sid, const std::string& body_user_id) {
    auto c = h_->Client();
    json inter = {{"namespace", kNs},
                  {"query_text", "Which database does the billing service use?"},
                  {"response_text", "The billing service stores invoices in PostgreSQL."}};
    if (!body_user_id.empty()) inter["user_id"] = body_user_id;
    auto wr = c.Post("/api/v1/memory/sessions/" + sid + "/interactions",
                     h_->Bearer(h_->user_key()), inter.dump(), "application/json");
    return wr ? wr->status : 0;
  }

  // Create a session owned by alice and append one interaction to it; returns the
  // session id (asserts both writes succeed).
  std::string WriteAliceInteraction(const std::string& body_user_id = "alice") {
    const std::string sid = CreateAliceSession();
    if (sid.empty()) return "";
    EXPECT_EQ(AppendInteraction(sid, body_user_id), 201);
    return sid;
  }

  int DeleteAliceSession(const std::string& sid) {
    auto c = h_->Client();
    auto res = c.Delete("/api/v1/memory/sessions/" + sid + "?namespace=" + kNs,
                        h_->Bearer(h_->user_key()));
    return res ? res->status : 0;
  }

  // Memory blocks (block_type 7) in the namespace's store: one per processed write.
  int MemoryBlockCount() {
    auto facade = h_->Facade(kNs);
    EXPECT_TRUE(facade->Acquire().ok());
    sqlite3* db = facade->store().db_handle();
    sqlite3_stmt* st = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM blocks WHERE block_type = 7", -1,
                                 &st, nullptr),
              SQLITE_OK);
    int n = 0;
    if (st && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
  }

  // POST /api/v1/memory/search as `key` for `user_id`; returns the results array.
  // `session_id` non-empty searches that session (scope=session) instead of the user.
  json Search(const std::string& key, const std::string& user_id,
              const std::string& session_id = "") {
    auto c = h_->Client();
    json body = {{"namespace", kNs},
                 {"user_id", user_id},
                 {"query", "billing service database"}};
    if (!session_id.empty()) {
      body["scope"] = "session";
      body["session_id"] = session_id;
    }
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

TEST_F(MemorySessionSearchE2E, OmittedUserIdFilesTheInteractionUnderTheSessionOwner) {
  ASSERT_FALSE(WriteAliceInteraction(/*body_user_id=*/"").empty());
  EXPECT_TRUE(WaitUntilAliceFindsIt());
}

TEST_F(MemorySessionSearchE2E, UserIdNamingSomeoneElseIsRejected) {
  // alice appends to her own session but claims to be mallory: the write must be
  // refused, so nothing can be planted in mallory's memory search.
  const std::string sid = CreateAliceSession();
  ASSERT_FALSE(sid.empty());
  EXPECT_EQ(AppendInteraction(sid, "mallory"), 400);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_EQ(MemoryBlockCount(), 0);
  EXPECT_FALSE(MentionsAnswer(Search(h_->other_key(), "mallory")));
}

TEST_F(MemorySessionSearchE2E, DeletedSessionIsNotSearchable) {
  const std::string sid = WriteAliceInteraction();
  ASSERT_FALSE(sid.empty());
  ASSERT_TRUE(WaitUntilAliceFindsIt());
  ASSERT_EQ(DeleteAliceSession(sid), 200);
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice")));
}

TEST_F(MemorySessionSearchE2E, WriteProcessedAfterDeleteIsNotSearchable) {
  // Delete the session right after the write is accepted, then wait until the
  // pipeline has written the block anyway: the search must still not return it.
  const std::string sid = CreateAliceSession();
  ASSERT_FALSE(sid.empty());
  ASSERT_EQ(AppendInteraction(sid, "alice"), 201);
  ASSERT_EQ(DeleteAliceSession(sid), 200);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (MemoryBlockCount() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_GT(MemoryBlockCount(), 0) << "the write was never processed";
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice")));
}

TEST_F(MemorySessionSearchE2E, RecreatedSessionIdDoesNotBringBackDeletedInteractions) {
  const std::string sid = "reused-session-1";
  ASSERT_EQ(CreateAliceSession(sid), sid);
  ASSERT_EQ(AppendInteraction(sid, "alice"), 201);
  ASSERT_TRUE(WaitUntilAliceFindsIt());
  ASSERT_EQ(DeleteAliceSession(sid), 200);
  ASSERT_EQ(CreateAliceSession(sid), sid);  // same id, new session
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice")));
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice", sid)));
  // The recreated session itself works: a new interaction in it is found.
  ASSERT_EQ(AppendInteraction(sid, "alice"), 201);
  EXPECT_TRUE(WaitUntilAliceFindsIt());
}

TEST_F(MemorySessionSearchE2E, WriteProcessedAfterDeleteAndRecreateIsNotSearchable) {
  const std::string sid = "reused-session-2";
  ASSERT_EQ(CreateAliceSession(sid), sid);
  ASSERT_EQ(AppendInteraction(sid, "alice"), 201);
  ASSERT_EQ(DeleteAliceSession(sid), 200);
  ASSERT_EQ(CreateAliceSession(sid), sid);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (MemoryBlockCount() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_GT(MemoryBlockCount(), 0) << "the write was never processed";
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice")));
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice", sid)));
}

}  // namespace
