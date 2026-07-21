/// @file test_multi_ns_fanout_e2e.cpp
/// @brief R7 full-stack E2E for F21 watcher fan-out over the PRODUCTION assembly
///        (real DirWatcherRegistry + real SPCManager + real PhnswIndexFactory pool),
///        driven through the connector HTTP routes.
///
/// Why this exists (full-stack, catching live-only seams):
/// the F21 fan-out unit test (tests/unit/test_dir_watcher_registry.cpp) drives the
/// private FanOutEvents with synthetic events over mock routers. This exercises the
/// LIVE path: a real file dropped in a watched directory, fanned out by the real
/// DirWatcherRegistry (lazily built by RegisterConnectorRoutes over the real F05 pool
/// + real catalog INSRouter + real SPCManager) to MULTIPLE namespaces, each landing
/// the document in its own per-NS store — all via the /connector HTTP routes.
///
/// It also carries the regression probe for the tenant_id FK bug this very suite
/// surfaced: the registry's NS auto-create (dir_watcher_registry.cpp) used to leave
/// NSMetadata.tenant_id empty, so units.tenant_id (NOT NULL, FK->tenants) failed and
/// a brand-new watch-dir namespace silently imported nothing. The fix sets
/// tenant_id="default_tenant". BareSubscribeNewNamespaceImportsFiles locks that in:
/// subscribing a never-before-seen namespace must import its files end-to-end.

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/connector/dir_watcher_registry.h"   // complete type for ConnectorState's unique_ptr dtor
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/routes/connector_routes.h"   // ConnectorState / RegisterConnectorRoutes
#include "cortrix/query/query_routes.h"                // RegisterQueryRoutes

#include "full_stack_e2e_harness.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// A short, separator-rich document (the StubTxtParser splits on blank lines). Content
// is irrelevant here — fan-out cares that the SAME file lands in every subscribed NS.
std::string SampleDoc(const std::string& tag) {
  return "Fan-out sample document " + tag +
         ". The watcher dispatches this file to every subscribed namespace.\n\n"
         "A second paragraph so the chunker produces a child block to store and query.";
}

class MultiNsFanoutE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    h_ = std::make_unique<cortrix::test::FullStackE2E>();
    h_->BuildIngest(/*embedding_dim=*/128);

    // The connector routes lazily build the DirWatcherRegistry over the real pool +
    // catalog router + SPCManager (the production wiring). NoAuth on these routes.
    cortrix::RegisterConnectorRoutes(h_->server(), conn_state_, h_->pool(),
                                     h_->ns_router(), h_->spc_mgr(), h_->auth());
    cortrix::RegisterQueryRoutes(h_->server(), h_->auth(), h_->pool(), h_->embedder(),
                                 h_->classifier(), h_->fusion());

    // A watched directory with two real files (created under the harness temp root so
    // teardown removes it; OUTSIDE the pool's unit dirs so importer scans don't see
    // store.db files).
    watch_dir_ = h_->root() / "watched";
    std::filesystem::create_directories(watch_dir_);
    WriteFile(watch_dir_ / "alpha.txt", SampleDoc("alpha"));
    WriteFile(watch_dir_ / "beta.txt", SampleDoc("beta"));

    h_->Start();
  }

  void WriteFile(const std::filesystem::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary);
    f << content;
  }

  // POST /connector/watchers — subscribe `dir` to `namespaces` (fan-out). Returns the
  // watcher id (asserts 200 + subscriber_count == namespaces.size()).
  std::string Subscribe(const std::string& dir,
                        const std::vector<std::string>& namespaces) {
    auto c = h_->Client();
    json body = {{"path", dir}, {"target_namespaces", namespaces}};
    // /connector/watchers is admin-gated (WithAuth kPermAdmin) -> send the admin key.
    auto res = c.Post("/api/v1/connector/watchers", h_->Bearer(h_->admin_key()),
                      body.dump(), "application/json");
    EXPECT_TRUE(res);
    EXPECT_EQ(res ? res->status : 0, 200) << (res ? res->body : "no response");
    if (!res || res->status != 200) return "";
    auto j = json::parse(res->body);
    EXPECT_EQ(j["subscriber_count"].get<int>(), static_cast<int>(namespaces.size()));
    return j["id"].get<std::string>();
  }

  // POST /connector/watchers/:id/scan — synchronous fan-out rescan (TriggerScan; no
  // dependence on OS-watcher event timing).
  void Scan(const std::string& id) {
    auto c = h_->Client();
    auto res = c.Post("/api/v1/connector/watchers/" + id + "/scan",
                      h_->Bearer(h_->admin_key()), "", "application/json");
    EXPECT_TRUE(res);
    EXPECT_EQ(res ? res->status : 0, 200) << (res ? res->body : "no response");
  }

  // Documents stored in a namespace (via its per-NS store).
  int64_t DocCount(const std::string& ns) {
    auto facade = h_->Facade(ns);
    EXPECT_TRUE(facade->Acquire().ok());
    int64_t n = 0;
    EXPECT_EQ(facade->store().doc_count(&n), 0);
    return n;
  }

  std::unique_ptr<cortrix::test::FullStackE2E> h_;
  cortrix::ConnectorState conn_state_;
  std::filesystem::path watch_dir_;
};

// One directory → two namespaces: after a scan, BOTH namespaces hold the directory's
// documents. This is the F21 fan-out core over the live registry + real pool.
TEST_F(MultiNsFanoutE2E, FanOutToMultipleNamespaces) {
  // Pre-create both namespaces OWNED BY the query principal (user_key == "alice")
  // so the per-NS query below passes V6 runtime authorization; this isolates the
  // assertion to fan-out, not NS creation (the bare-create path has its own probe).
  ASSERT_TRUE(h_->CreateNamespaceOwnedBy("team_a", "alice").ok());
  ASSERT_TRUE(h_->CreateNamespaceOwnedBy("team_b", "alice").ok());

  const std::string id = Subscribe(watch_dir_.string(), {"team_a", "team_b"});
  ASSERT_FALSE(id.empty());
  Scan(id);

  // Both NS imported the two files (the single directory fanned out to each).
  EXPECT_GE(DocCount("team_a"), 2) << "team_a did not receive the watched files";
  EXPECT_GE(DocCount("team_b"), 2) << "team_b did not receive the watched files";

  // Each NS is independently queryable (per-NS store landed the content).
  for (const char* ns : {"team_a", "team_b"}) {
    auto c = h_->Client();
    json q = {{"query", "fan-out sample"}, {"namespace", ns}, {"top_k", 5}};
    auto res = c.Post("/api/v1/query", h_->Bearer(h_->user_key()), q.dump(),
                      "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
  }
}

// Unsubscribing one namespace from a watcher leaves the other subscribed (per-NS
// subscription lifecycle; the watcher survives until its last subscriber leaves).
TEST_F(MultiNsFanoutE2E, PerNamespaceUnsubscribeKeepsOthers) {
  ASSERT_TRUE(h_->CreateNamespace("team_a").ok());
  ASSERT_TRUE(h_->CreateNamespace("team_b").ok());
  const std::string id = Subscribe(watch_dir_.string(), {"team_a", "team_b"});
  ASSERT_FALSE(id.empty());

  auto c = h_->Client();
  auto del = c.Delete("/api/v1/connector/watchers/" + id + "/namespaces/team_b",
                      h_->Bearer(h_->admin_key()));
  ASSERT_TRUE(del);
  EXPECT_EQ(del->status, 200) << del->body;

  // The watcher still exists with team_a as its remaining subscriber.
  auto got = c.Get("/api/v1/connector/watchers", h_->Bearer(h_->admin_key()));
  ASSERT_TRUE(got);
  ASSERT_EQ(got->status, 200) << got->body;
  auto j = json::parse(got->body);
  bool found_team_a = false, found_team_b = false;
  for (const auto& w : j["watchers"]) {
    for (const auto& ns : w["target_namespaces"]) {
      if (ns == "team_a") found_team_a = true;
      if (ns == "team_b") found_team_b = true;
    }
  }
  EXPECT_TRUE(found_team_a) << "team_a should remain subscribed";
  EXPECT_FALSE(found_team_b) << "team_b should have been unsubscribed";
}

// REGRESSION PROBE (tenant_id FK fix): subscribing a brand-new namespace that was
// NEVER pre-created must still import the watched files. Before the fix, the
// registry's NS auto-create left tenant_id empty → units.tenant_id FK->tenants
// failed → the NS was never admitted → facade.Acquire() failed → zero files
// imported (silently). With tenant_id="default_tenant" the NS is created + admitted
// and the files flow. (Red/green proof: `git stash` the registry fix → this fails
// with DocCount==0; `git stash pop` → it passes.)
TEST_F(MultiNsFanoutE2E, BareSubscribeNewNamespaceImportsFiles) {
  // NOTE: "fresh_team" is deliberately NOT pre-created — the registry's own
  // auto-create path (the one that carried the bug) is what must create + admit it.
  const std::string id = Subscribe(watch_dir_.string(), {"fresh_team"});
  ASSERT_FALSE(id.empty());
  Scan(id);

  EXPECT_GE(DocCount("fresh_team"), 2)
      << "bare-subscribed namespace imported no files — the registry NS auto-create "
         "did not create+admit the namespace (tenant_id FK regression)";
}

}  // namespace
}  // namespace cortrix
