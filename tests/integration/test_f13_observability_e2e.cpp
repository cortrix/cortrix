/// @file test_f13_observability_e2e.cpp
/// @brief R7 full-stack E2E for Agent Observability + Operation Log over the
///        PRODUCTION live wiring (cross-DB owner resolution).
///
/// Why this exists (full-stack integration, not component-level):
/// the existing agent trace coverage (tests/unit/test_observability_routes.cpp) registers the
/// *co-located* RegisterObservabilityRoutes — agent_trace and interaction_log share one
/// DB. Production does NOT: bootstrap wires RegisterTracesRoutesGlobal (agent_trace in
/// the GLOBAL cortrix_global.db) + RegisterInteractionsRoutesPerNs (interaction_log in
/// each namespace's memory.db). The §8.1 permission check therefore runs a CROSS-DB
/// owner resolver (ResolveGlobalTraceOwner): read the session's namespace_id off the
/// global agent_trace, then look up user_id in THAT namespace's memory.db. That live
/// assembly seam — the exact D3.5 live-only class of bug — has never been E2E-covered.
///
/// This test stands up the production assembly via FullStackE2E (real CatalogDb global
/// db + real DefaultINSRouter/Pool + real PhnswIndexFactory), seeds an owner via the
/// per-NS memory.db through a real NamespaceFacade, writes agent_trace rows to the
/// global db through the real AgentTraceWriterImpl, registers the three live route
/// groups, and drives them over a real httplib server:
///   - GET /api/v1/traces/{session_id}        (global trace read; cross-DB owner check)
///   - GET /api/v1/interactions               (per-NS list)
///   - GET /api/v1/operations                 (operation_log over the global db)

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/agent_trace/agent_trace_writer.h"        // AgentTraceEntry
#include "cortrix/agent_trace/agent_trace_writer_impl.h"   // AgentTraceWriterImpl
#include "cortrix/memory/interaction_log.h"                // InteractionLog
#include "cortrix/memory/memory_session.h"                 // MemorySession
#include "cortrix/observability/operation_logger.h"        // IOperationLogger / OperationLogEntry
#include "cortrix/observability/operation_logger_impl.h"   // OperationLogger (concrete)
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/routes/observability_routes.h"    // RegisterTracesRoutesGlobal / *PerNs
#include "cortrix/server/routes/operations_routes.h"       // RegisterOperationsRoutes

#include "full_stack_e2e_harness.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using cortrix::agent_trace::AgentTraceEntry;
using cortrix::agent_trace::AgentTraceWriterImpl;

// The two namespaces this suite admits. "sales" is alice's (the owner under test);
// "eng" exists so cross-NS / wrong-NS behavior is meaningful.
constexpr const char* kNsSales = "sales";
constexpr const char* kNsEng = "eng";

class F13ObservabilityE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    h_ = std::make_unique<cortrix::test::FullStackE2E>();
    h_->BuildCore();

    // Admit both namespaces through the REAL catalog router (catalog INSERT + namespace pool
    // AdmitCreate) so each gets a real per-NS memory.db + store + index.
    ASSERT_TRUE(h_->CreateNamespace(kNsSales).ok());
    ASSERT_TRUE(h_->CreateNamespace(kNsEng).ok());

    // ── Seed the owner mapping in the per-NS memory.db (the cross-DB owner source).
    // A NamespaceFacade::Acquire() opens <unit>/memory.db and migrates interaction_log
    // + memory_sessions. We create alice's session there and log one interaction; the
    // session_id is auto-generated, so capture it for the matching agent_trace rows.
    {
      auto facade = h_->Facade(kNsSales);
      ASSERT_TRUE(facade->Acquire().ok());
      MemorySession s;
      s.namespace_name = kNsSales;
      s.user_id = "alice";
      ASSERT_TRUE(facade->memory().SessionCreate(s).ok());
      alice_session_ = s.session_id;
      ASSERT_FALSE(alice_session_.empty());

      InteractionLog log;
      log.session_id = alice_session_;
      log.namespace_name = kNsSales;
      log.user_id = "alice";
      log.role = "user";
      log.content = "what is the Q3 revenue?";
      log.query_type = "semantic";
      ASSERT_TRUE(facade->memory().InteractionInsert(log).ok());
    }

    // ── Seed agent_trace in the GLOBAL db (where RegisterTracesRoutesGlobal reads).
    // Each row stamps namespace_id="sales" so the owner resolver knows which NS
    // memory.db to consult. session_id matches the interaction above.
    {
      AgentTraceWriterImpl writer(h_->global_db(), h_->global_config());
      for (int i = 0; i < 3; ++i) {
        AgentTraceEntry e;
        e.session_id = alice_session_;
        e.namespace_id = std::string(kNsSales);
        e.method = "cortrix_query";
        e.source = "mcp";
        e.duration_ms = 10 + i;
        e.created_at = 1000 + i;
        writer.Write(e);
      }
    }

    // ── Seed operation_log in the GLOBAL db. alice has rows, bob has one.
    {
      op_logger_ = std::make_shared<cortrix::observability::OperationLogger>(
          h_->global_db(), h_->global_config());
      SeedOp("alice", "memory_create", "memory", kNsSales, "blk-1", 2000);
      SeedOp("alice", "query", "query", kNsSales, "ix-1", 3000);
      SeedOp("bob", "document_upload", "document", kNsEng, "doc-7", 2500);
    }

    // ── Register the THREE production route groups onto the real server.
    cortrix::RegisterTracesRoutesGlobal(h_->server(), h_->global_db(), h_->pool(),
                                        h_->global_config(), h_->auth());
    cortrix::RegisterInteractionsRoutesPerNs(h_->server(), h_->pool(), h_->auth());
    cortrix::RegisterOperationsRoutes(h_->server(), *op_logger_, h_->auth());

    h_->Start();
  }

  void SeedOp(const std::string& user, const std::string& action,
              const std::string& rtype, const std::string& ns,
              const std::string& rid, int64_t ts) {
    cortrix::observability::OperationLogEntry e;
    e.timestamp = ts;
    e.user_id = user;
    e.action = action;
    e.resource_type = rtype;
    e.namespace_id = ns;
    e.resource_id = rid;
    op_logger_->Log(e);
  }

  std::unique_ptr<cortrix::test::FullStackE2E> h_;
  std::shared_ptr<cortrix::observability::OperationLogger> op_logger_;
  std::string alice_session_;
};

// ── GET /api/v1/traces/{session_id} — global read + cross-DB owner resolution ──────

// The owner (alice) reads her own session: the resolver reads namespace_id="sales"
// off the global agent_trace, then resolves user_id="alice" from the sales NS
// memory.db — a match, so the 3 trace rows come back. This is the cross-DB seam.
TEST_F(F13ObservabilityE2E, OwnerReadsOwnSessionTraces) {
  auto c = h_->Client();
  auto res = c.Get("/api/v1/traces/" + alice_session_, h_->Bearer(h_->user_key()));
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto body = json::parse(res->body);
  EXPECT_EQ(body["session_id"], alice_session_);
  EXPECT_EQ(body["trace_count"], 3);
  ASSERT_TRUE(body["traces"].is_array());
  EXPECT_EQ(body["traces"].size(), 3u);
  for (const auto& t : body["traces"]) {
    EXPECT_EQ(t["session_id"], alice_session_);
    EXPECT_EQ(t["namespace_id"], kNsSales);
  }
}

// An admin reads any session regardless of ownership (§8.1).
TEST_F(F13ObservabilityE2E, AdminReadsAnySessionTraces) {
  auto c = h_->Client();
  auto res = c.Get("/api/v1/traces/" + alice_session_, h_->Bearer(h_->admin_key()));
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto body = json::parse(res->body);
  EXPECT_EQ(body["trace_count"], 3);
}

// A non-owner non-admin (mallory) is denied by the anti-leak rule. The resolver
// finds the session belongs to "alice" (cross-DB), mallory != alice + not admin ->
// CX_ERR_F13_UNAUTHORIZED. This is the core assembly assertion: a co-located-only
// resolver would never reach the sales NS memory.db to learn the owner is alice.
TEST_F(F13ObservabilityE2E, NonOwnerNonAdminDeniedTraces) {
  auto c = h_->Client();
  auto res = c.Get("/api/v1/traces/" + alice_session_, h_->Bearer(h_->other_key()));
  ASSERT_TRUE(res);
  EXPECT_NE(res->status, 200);
  auto body = json::parse(res->body);
  ASSERT_TRUE(body.contains("error"));
  EXPECT_EQ(body["error"]["code"], "CX_ERR_F13_UNAUTHORIZED");
}

// No credentials -> 401 at the auth gate (before any handler logic).
TEST_F(F13ObservabilityE2E, TracesRequireAuth) {
  auto c = h_->Client();
  auto res = c.Get("/api/v1/traces/" + alice_session_);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

// An admin querying a session with no traces gets SESSION_NOT_FOUND (the session
// resolves to no namespace -> owner unknown -> NOT_FOUND for an admin, §8.1).
TEST_F(F13ObservabilityE2E, AdminUnknownSessionNotFound) {
  auto c = h_->Client();
  auto res = c.Get("/api/v1/traces/no-such-session", h_->Bearer(h_->admin_key()));
  ASSERT_TRUE(res);
  EXPECT_NE(res->status, 200);
  auto body = json::parse(res->body);
  ASSERT_TRUE(body.contains("error"));
  EXPECT_EQ(body["error"]["code"], "CX_ERR_F13_SESSION_NOT_FOUND");
}

// Trace filter (status) reaches the writer query: filtering to a status none of the
// seeded rows carry yields an empty page (the rows are status="success").
TEST_F(F13ObservabilityE2E, TracesStatusFilterApplies) {
  auto c = h_->Client();
  auto res = c.Get("/api/v1/traces/" + alice_session_ + "?status=failed",
                   h_->Bearer(h_->user_key()));
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto body = json::parse(res->body);
  EXPECT_EQ(body["trace_count"], 0);
}

// ── GET /api/v1/interactions — per-NS list over the namespace memory.db ────────────

// alice lists interactions in the sales NS (namespace_id selects the per-NS
// memory.db): her seeded interaction comes back. Verifies RegisterInteractionsRoutesPerNs
// selected the right per-NS DB via the namespace_id param.
TEST_F(F13ObservabilityE2E, ListInteractionsPerNamespace) {
  auto c = h_->Client();
  auto res = c.Get(std::string("/api/v1/interactions?namespace_id=") + kNsSales,
                   h_->Bearer(h_->user_key()));
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto body = json::parse(res->body);
  ASSERT_TRUE(body["interactions"].is_array());
  ASSERT_GE(body["interactions"].size(), 1u);
  bool found = false;
  for (const auto& it : body["interactions"]) {
    if (it["session_id"] == alice_session_) {
      found = true;
      EXPECT_EQ(it["user_id"], "alice");
      EXPECT_EQ(it["namespace_id"], kNsSales);
    }
  }
  EXPECT_TRUE(found);
}

// A non-admin naming someone else's user_id in the interactions filter is denied
// (§8.3 mirrors §8.2 anti-leak).
TEST_F(F13ObservabilityE2E, ListInteractionsCrossUserDenied) {
  auto c = h_->Client();
  auto res =
      c.Get(std::string("/api/v1/interactions?namespace_id=") + kNsSales +
                "&user_id=alice",
            h_->Bearer(h_->other_key()));
  ASSERT_TRUE(res);
  EXPECT_NE(res->status, 200);
  auto body = json::parse(res->body);
  ASSERT_TRUE(body.contains("error"));
  EXPECT_EQ(body["error"]["code"], "CX_ERR_F13_UNAUTHORIZED");
}

// The per-NS list requires the namespace selector (per-NS storage cannot be picked
// without it) -> route-side INVALID_FILTER.
TEST_F(F13ObservabilityE2E, ListInteractionsRequiresNamespace) {
  auto c = h_->Client();
  auto res = c.Get("/api/v1/interactions", h_->Bearer(h_->user_key()));
  ASSERT_TRUE(res);
  EXPECT_NE(res->status, 200);
  auto body = json::parse(res->body);
  ASSERT_TRUE(body.contains("error"));
  EXPECT_EQ(body["error"]["code"], "CX_ERR_F13_INVALID_FILTER");
}

// ── GET /api/v1/operations — operation_log over the global db ──────────────────────
// (Smoke that the three observability route groups coexist on one live server and the
//  Operation log query path works against the same global db the trace writer used.)

// alice sees only her own operation rows by default (self-scope).
TEST_F(F13ObservabilityE2E, OperationsSelfScope) {
  auto c = h_->Client();
  auto res = c.Get("/api/v1/operations", h_->Bearer(h_->user_key()));
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;
  auto body = json::parse(res->body);
  ASSERT_TRUE(body["operations"].is_array());
  ASSERT_GE(body["operations"].size(), 1u);
  for (const auto& op : body["operations"]) {
    EXPECT_EQ(op["user_id"], "alice");
  }
}

// A non-admin naming another user_id in the operations filter is unauthorized
// (operation log cross-user gate).
TEST_F(F13ObservabilityE2E, OperationsCrossUserDenied) {
  auto c = h_->Client();
  auto res = c.Get("/api/v1/operations?user_id=bob", h_->Bearer(h_->user_key()));
  ASSERT_TRUE(res);
  EXPECT_NE(res->status, 200);
  auto body = json::parse(res->body);
  ASSERT_TRUE(body.contains("error"));
  EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_UNAUTHORIZED");
}

}  // namespace
}  // namespace cortrix
