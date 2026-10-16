// POST /api/v1/namespaces/:name/enrich/backfill through the real in-process
// httplib server over a real per-Unit store (NsPoolHarness): the body contract
// (strict object with two boolean flags), the counts that every 200 carries, the
// operator reset wired end to end against the store's own enrich_state and
// blocks tables, and the audit's removal of contextual index entries the
// ingest's rule excludes, reported in the response and the operation log.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/routes/enrich_routes.h"
#include "cortrix/spc_enricher/enrich_state_store.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
namespace obs = cortrix::observability;
using json = nlohmann::json;

constexpr const char* kPath = "/api/v1/namespaces/default/enrich/backfill";

// Records what the route logs; nothing else is exercised.
class CapturingOpLogger : public obs::IOperationLogger {
public:
    void Log(const obs::OperationLogEntry& entry, const obs::TraceContext* = nullptr) override {
        entries.push_back(entry);
    }
    void BatchLog(const std::vector<obs::OperationLogEntry>& es,
                  const obs::TraceContext* = nullptr) override {
        for (const auto& e : es) entries.push_back(e);
    }
    Result<obs::OperationLogQueryResult> Query(const obs::OperationLogFilter&,
                                               const obs::TraceContext* = nullptr) override {
        return obs::OperationLogQueryResult{};
    }
    void Cleanup() override {}
    obs::OperationLogStats GetStats() override { return {}; }
    obs::HealthStatus Health() override { return {}; }

    std::vector<obs::OperationLogEntry> entries;
};

class EnrichRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() /
                    ("cortrix_enrich_routes_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(temp_dir_ / "pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        // Auth disabled: WithAuth(auth, kPermAdmin, ...) grants admin and passes
        // through, which is the deployment mode these tests model; the
        // permission gate itself is covered by the auth middleware tests.
        auth_ = std::make_unique<ApiKeyAuth>();
        auth_->set_enabled(false);

        server_ = std::make_unique<httplib::Server>();
        // No sweeper (the route reports enqueued=0 then): the contract under test is
        // the body handling, the ledger changes and what the route reports.
        RegisterEnrichRoutes(*server_, harness_->ipool(), *auth_, /*sweeper=*/nullptr,
                             {"f03", "f35", "f38"}, &oplog_);

        port_ = server_->bind_to_any_port("127.0.0.1");
        ASSERT_GT(port_, 0);
        server_thread_ = std::thread([this]() { server_->listen_after_bind(); });
        for (int i = 0; i < 50 && !server_->is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(server_->is_running());
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        facade_.reset();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    // Borrow the namespace's own store handle, as the route does.
    sqlite3* Db() {
        facade_ = std::make_unique<resource::NamespaceFacade>(harness_->ipool(), "default");
        if (!facade_->Acquire().ok()) return nullptr;
        return facade_->store().db_handle();
    }

    void Exec(sqlite3* db, const std::string& sql) {
        char* err = nullptr;
        const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
        ASSERT_EQ(rc, SQLITE_OK) << (err ? err : "") << " :: " << sql;
    }

    // A document with one real child block (the store's NOT NULL columns filled),
    // so the reset can tell a live chunk from a vanished one.
    void SeedDocAndBlock(sqlite3* db, const std::string& doc_id, uint64_t block_id) {
        Exec(db, "INSERT INTO documents(doc_id, source_type, source_path, status)"
                 " VALUES('" + doc_id + "','file','/x/" + doc_id + "','processed')");
        Exec(db, "INSERT INTO blocks(block_id, doc_id, chunk_index, block_type,"
                 " processing_level, data, child_id) VALUES(" + std::to_string(block_id) +
                 ",'" + doc_id + "',0,1,3,X'00','child-" + std::to_string(block_id) + "')");
    }

    void SeedGivenUp(sqlite3* db, uint64_t block_id, const std::string& doc_id) {
        spc::EnrichStateRow r;
        r.block_id = block_id;
        r.doc_id = doc_id;
        r.child_id = "child-" + std::to_string(block_id);
        r.status = spc::kEnrichStatusFailedPermanent;
        r.failed_members = "f03,f35,f38";
        r.attempts = spc::kEnrichMaxAttempts;
        r.last_error = "provider timeout";
        r.updated_at = 1000;
        ASSERT_TRUE(spc::UpsertEnrichState(db, r).ok());
    }

    int Count(sqlite3* db, const std::string& sql) {
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr), SQLITE_OK) << sql;
        int n = -1;
        if (st && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        return n;
    }

    // One document with one child block at the given processing level, its
    // metadata_json (nullptr → NULL), and a contextual label row for the block.
    void SeedLabelledChunk(sqlite3* db, const std::string& doc_id, int level,
                           uint64_t block_id, const char* metadata_json, uint64_t label) {
        Exec(db, "INSERT INTO documents(doc_id, source_type, source_path, status,"
                 " processing_level) VALUES('" + doc_id + "','file','/x/" + doc_id +
                 "','ready'," + std::to_string(level) + ")");
        const std::string child = "child-" + std::to_string(block_id);
        const std::string meta = metadata_json ? std::string("'") + metadata_json + "'" : "NULL";
        Exec(db, "INSERT INTO blocks(block_id, doc_id, chunk_index, block_type,"
                 " processing_level, data, child_id, contextualized_status, metadata_json)"
                 " VALUES(" + std::to_string(block_id) + ",'" + doc_id + "',0,1," +
                 std::to_string(level) + ",X'00','" + child + "',1," + meta + ")");
        Exec(db, "INSERT INTO contextual_vec_labels(label, block_id, child_id) VALUES(" +
                 std::to_string(label) + "," + std::to_string(block_id) + ",'" + child + "')");
    }

    std::string LastSummary() const {
        if (oplog_.entries.empty() || !oplog_.entries.back().summary) return "<none>";
        return *oplog_.entries.back().summary;
    }

    httplib::Result Post(const std::string& body) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        if (body.empty()) return cli.Post(kPath);
        return cli.Post(kPath, body, "application/json");
    }

    static void ExpectInvalidBody(const httplib::Result& res) {
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 400) << res->body;
        auto j = json::parse(res->body, nullptr, false);
        ASSERT_FALSE(j.is_discarded()) << res->body;
        EXPECT_EQ(j["error"]["code"], "CX_ERR_ENRICH_BACKFILL_INVALID_BODY") << res->body;
    }

    fs::path temp_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::unique_ptr<resource::NamespaceFacade> facade_;
    std::unique_ptr<ApiKeyAuth> auth_;
    CapturingOpLogger oplog_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
};

// No body: audit on, reset off, and every count present with its value.
TEST_F(EnrichRoutesTest, EmptyBodyRunsAuditAndReportsEveryCount) {
    auto res = Post("");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["namespace"], "default");
    EXPECT_EQ(j["audited"], true);
    EXPECT_EQ(j["reset_given_up"], false);
    EXPECT_EQ(j["synthesized"], 0);
    EXPECT_EQ(j["settled"], 0);
    EXPECT_EQ(j["reset"], 0);
    EXPECT_EQ(j["orphaned_skipped"], 0);
    EXPECT_EQ(j["enqueued"], 0);
    ASSERT_TRUE(j.contains("counts")) << res->body;
    EXPECT_EQ(j["counts"]["total"], 0);
}

// The body must be a JSON object: a bare array, a string, or broken JSON is a
// named 400, not a silent "nothing to do".
TEST_F(EnrichRoutesTest, RejectsBodyThatIsNotAnObject) {
    ExpectInvalidBody(Post("[]"));
    ExpectInvalidBody(Post("\"audit\""));
    ExpectInvalidBody(Post("{not json"));
}

// A key the route does not know is rejected; a misspelled flag must not pass.
TEST_F(EnrichRoutesTest, RejectsUnknownField) {
    ExpectInvalidBody(Post(R"({"audut": true})"));
    ExpectInvalidBody(Post(R"({"audit": true, "reset": true})"));
}

// The two flags are booleans; "true" the string is rejected.
TEST_F(EnrichRoutesTest, RejectsNonBooleanFlag) {
    ExpectInvalidBody(Post(R"({"audit": "true"})"));
    ExpectInvalidBody(Post(R"({"reset_given_up": 1})"));
}

// Both flags off is a valid request: nothing audited, nothing reset, all counts 0.
TEST_F(EnrichRoutesTest, BothFlagsOffIsValidAndReportsZeroes) {
    auto res = Post(R"({"audit": false, "reset_given_up": false})");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["audited"], false);
    EXPECT_EQ(j["reset_given_up"], false);
    EXPECT_EQ(j["synthesized"], 0);
    EXPECT_EQ(j["settled"], 0);
    EXPECT_EQ(j["reset"], 0);
    EXPECT_EQ(j["orphaned_skipped"], 0);
}

// The reset end to end: one given-up row whose chunk exists moves back to
// pending with a fresh count; one whose chunk is gone stays terminal and is
// reported in orphaned_skipped; the namespace counts reflect both.
TEST_F(EnrichRoutesTest, ResetGivenUpMovesLiveRowsAndReportsOrphans) {
    sqlite3* db = Db();
    ASSERT_NE(db, nullptr);
    SeedDocAndBlock(db, "doc-live", 501);
    SeedGivenUp(db, 501, "doc-live");
    SeedGivenUp(db, 502, "doc-gone");  // no blocks row: the chunk vanished

    auto res = Post(R"({"audit": false, "reset_given_up": true})");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["audited"], false);
    EXPECT_EQ(j["reset_given_up"], true);
    EXPECT_EQ(j["reset"], 1);
    EXPECT_EQ(j["orphaned_skipped"], 1);
    EXPECT_EQ(j["settled"], 0);
    EXPECT_EQ(j["counts"]["total"], 2);
    EXPECT_EQ(j["counts"]["pending_retry"], 1);
    EXPECT_EQ(j["counts"]["failed_permanent"], 1);

    auto live = spc::ListEnrichStateForDoc(db, "doc-live", "");
    ASSERT_TRUE(live.ok());
    ASSERT_EQ(live.value().size(), 1u);
    EXPECT_EQ(live.value()[0].status, spc::kEnrichStatusPendingRetry);
    EXPECT_EQ(live.value()[0].attempts, 0);
    EXPECT_EQ(live.value()[0].last_error, "reset by operator");
    EXPECT_GT(live.value()[0].next_retry_at, 0);

    auto gone = spc::ListEnrichStateForDoc(db, "doc-gone", "");
    ASSERT_TRUE(gone.ok());
    ASSERT_EQ(gone.value().size(), 1u);
    EXPECT_EQ(gone.value()[0].status, spc::kEnrichStatusFailedPermanent);
    EXPECT_EQ(gone.value()[0].attempts, spc::kEnrichMaxAttempts);
}

// The audit's settle end to end: a given-up row whose chunk carries every
// configured artifact (score, contextual columns and label, a hype block) is
// settled to ok by the default request and reported in `settled`.
TEST_F(EnrichRoutesTest, AuditSettlesCompleteGivenUpRowAndReportsIt) {
    sqlite3* db = Db();
    ASSERT_NE(db, nullptr);
    SeedDocAndBlock(db, "doc-done", 521);
    Exec(db, "UPDATE blocks SET enriched_score=0.5, contextualized_status=1"
             " WHERE block_id=521");
    Exec(db, "INSERT INTO contextual_vec_labels(label, block_id, child_id)"
             " VALUES(1, 521, 'child-521')");
    Exec(db, "INSERT INTO blocks(block_id, doc_id, chunk_index, block_type, processing_level,"
             " data, metadata_json) VALUES(9521,'doc-done',1,16,3,X'00',"
             "'{\"source_child_id\":\"child-521\"}')");
    SeedGivenUp(db, 521, "doc-done");

    auto res = Post("{}");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["audited"], true);
    EXPECT_EQ(j["synthesized"], 0);
    EXPECT_EQ(j["settled"], 1);
    EXPECT_EQ(j["reset"], 0);
    EXPECT_EQ(j["counts"]["ok"], 1);
    EXPECT_EQ(j["counts"]["failed_permanent"], 0);

    auto rows = spc::ListEnrichStateForDoc(db, "doc-done", "");
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(rows.value()[0].status, spc::kEnrichStatusOk);
    EXPECT_EQ(rows.value()[0].attempts, spc::kEnrichMaxAttempts);  // history kept
    EXPECT_EQ(rows.value()[0].failed_members, "");
}

// Without the flag the same ledger is left alone: the reset is opt-in.
TEST_F(EnrichRoutesTest, DefaultRequestDoesNotResetGivenUpRows) {
    sqlite3* db = Db();
    ASSERT_NE(db, nullptr);
    SeedDocAndBlock(db, "doc-live", 511);
    SeedGivenUp(db, 511, "doc-live");

    auto res = Post(R"({"audit": false})");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["reset_given_up"], false);
    EXPECT_EQ(j["reset"], 0);
    EXPECT_EQ(j["orphaned_skipped"], 0);
    EXPECT_EQ(j["counts"]["failed_permanent"], 1);

    auto rows = spc::ListEnrichStateForDoc(db, "doc-live", "");
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(rows.value()[0].status, spc::kEnrichStatusFailedPermanent);
    EXPECT_EQ(rows.value()[0].attempts, spc::kEnrichMaxAttempts);
}

// The default audit removes exactly the contextual index entries the ingest's rule
// excludes (a skip-index chunk's, a level-2 document's chunk) and says how many, in
// the response and the operation log; a second audit finds nothing left; every
// other entry stays.
TEST_F(EnrichRoutesTest, AuditRemovesExcludedContextualEntriesAndReportsRemoved) {
    sqlite3* db = Db();
    ASSERT_NE(db, nullptr);
    SeedLabelledChunk(db, "doc-skip", 3, 601, R"({"cleaning.skip_index":true})", 7601);
    SeedLabelledChunk(db, "doc-l2", 2, 602, nullptr, 7602);
    SeedLabelledChunk(db, "doc-plain", 3, 603, nullptr, 7603);
    ASSERT_EQ(Count(db, "SELECT COUNT(*) FROM contextual_vec_labels"), 3);  // positive control
    ASSERT_NE(harness_->fake_index(), nullptr);
    ASSERT_TRUE(harness_->fake_index()->deleted_ids().empty());

    auto res = Post("");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["audited"], true);
    EXPECT_EQ(j["removed"], 2) << res->body;
    EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM contextual_vec_labels"
                        " WHERE label IN (7601, 7602)"), 0);
    EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM contextual_vec_labels WHERE label=7603"), 1);
    std::vector<uint64_t> deleted = harness_->fake_index()->deleted_ids();
    std::sort(deleted.begin(), deleted.end());
    EXPECT_EQ(deleted, (std::vector<uint64_t>{7601, 7602}))
        << "the excluded points were marked deleted through the namespace's index";
    EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM blocks WHERE block_id IN (601, 602)"
                        " AND contextualized_status = 1"), 2)
        << "the chunks keep their contextual columns";
    ASSERT_EQ(oplog_.entries.size(), 1u);
    EXPECT_EQ(oplog_.entries.back().action, "enrich_backfill");
    EXPECT_EQ(LastSummary(), "removed=2 synthesized=" + std::to_string(j["synthesized"].get<int>()) +
                                 " settled=0 reset=0 orphaned=0 enqueued=0");

    auto again = Post("");
    ASSERT_TRUE(again);
    ASSERT_EQ(again->status, 200) << again->body;
    EXPECT_EQ(json::parse(again->body)["removed"], 0);
    EXPECT_EQ(harness_->fake_index()->deleted_ids().size(), 2u);
    EXPECT_EQ(LastSummary().rfind("removed=0 ", 0), 0u) << LastSummary();
}

// With the audit off nothing is reconciled, and `removed` is reported as 0.
TEST_F(EnrichRoutesTest, AuditOffRemovesNothing) {
    sqlite3* db = Db();
    ASSERT_NE(db, nullptr);
    SeedLabelledChunk(db, "doc-skip", 3, 611, R"({"cleaning.skip_index":true})", 7611);

    auto res = Post(R"({"audit": false})");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["audited"], false);
    EXPECT_EQ(j["removed"], 0);
    EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM contextual_vec_labels"), 1);
    EXPECT_TRUE(harness_->fake_index()->deleted_ids().empty());
    EXPECT_EQ(LastSummary(), "removed=0 synthesized=0 settled=0 reset=0 orphaned=0 enqueued=0");
}

}  // namespace
}  // namespace cortrix
