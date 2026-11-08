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
#include <condition_variable>
#include <memory>
#include <mutex>
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
    // Every memory-session write passes this gate just before its block is written.
    // It is open unless a test closes it to hold a write while it deletes or
    // recreates the session.
    h_->spc_mgr().SetMemoryBlockWriteHook([this](const cortrix::SPCTask&) {
      std::unique_lock<std::mutex> lk(gate_mu_);
      ++gate_arrivals_;
      gate_cv_.notify_all();
      gate_cv_.wait(lk, [this] { return gate_open_; });
    });
    // Owned by the tenant of user_key() ("alice"), so alice's requests pass the
    // namespace check on the routes that have one.
    ASSERT_TRUE(h_->CreateNamespaceOwnedBy(kNs, "alice").ok());
    cortrix::RegisterMemoryRoutes(h_->server(), h_->auth(), h_->pool(), h_->spc_mgr(),
                                  h_->embedder(), h_->classifier(), h_->fusion(),
                                  config_);
    h_->Start();
  }

  void TearDown() override { OpenGate(); }  // never leave a worker parked at the gate

  void CloseGate() {
    std::lock_guard<std::mutex> lk(gate_mu_);
    gate_open_ = false;
  }
  void OpenGate() {
    std::lock_guard<std::mutex> lk(gate_mu_);
    gate_open_ = true;
    gate_cv_.notify_all();
  }
  // Wait until a write is parked at the closed gate, i.e. its block is not written yet.
  bool WaitForWriteAtGate(int timeout_ms = 10000) {
    std::unique_lock<std::mutex> lk(gate_mu_);
    return gate_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                             [this] { return gate_arrivals_ > 0; });
  }
  // After opening the gate: wait until the held write's block is in the store.
  bool WaitForMemoryBlock(int timeout_ms = 10000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (MemoryBlockCount() > 0) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
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
  // Declared before h_ so the harness (and its workers) is destroyed first.
  std::mutex gate_mu_;
  std::condition_variable gate_cv_;
  bool gate_open_ = true;
  int gate_arrivals_ = 0;
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
  // Hold the write before its block is written, delete the session, then let the
  // write finish: the block lands after the delete and must not be returned.
  CloseGate();
  const std::string sid = CreateAliceSession();
  ASSERT_FALSE(sid.empty());
  ASSERT_EQ(AppendInteraction(sid, "alice"), 201);
  ASSERT_TRUE(WaitForWriteAtGate()) << "the write never reached the block write";
  EXPECT_EQ(MemoryBlockCount(), 0);
  ASSERT_EQ(DeleteAliceSession(sid), 200);
  OpenGate();
  ASSERT_TRUE(WaitForMemoryBlock()) << "the held write was never written";
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice")));
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice", sid)));
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
  // Hold the write, delete the session and create it again with the same id, then
  // let the write finish: neither the user nor the recreated session sees it.
  const std::string sid = "reused-session-2";
  CloseGate();
  ASSERT_EQ(CreateAliceSession(sid), sid);
  ASSERT_EQ(AppendInteraction(sid, "alice"), 201);
  ASSERT_TRUE(WaitForWriteAtGate()) << "the write never reached the block write";
  EXPECT_EQ(MemoryBlockCount(), 0);
  ASSERT_EQ(DeleteAliceSession(sid), 200);
  ASSERT_EQ(CreateAliceSession(sid), sid);
  OpenGate();
  ASSERT_TRUE(WaitForMemoryBlock()) << "the held write was never written";
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice")));
  EXPECT_FALSE(MentionsAnswer(Search(h_->user_key(), "alice", sid)));
}

}  // namespace
