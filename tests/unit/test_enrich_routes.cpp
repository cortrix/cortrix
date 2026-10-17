// POST /api/v1/namespaces/:name/enrich/backfill through the real in-process
// httplib server over a real per-Unit store (NsPoolHarness): the default audit
// removes the contextual index entries the ingest's rule excludes (a skip-index
// chunk's, a chunk's whose document is below the full processing level), marks
// their points deleted through the namespace's own index, keeps every other
// entry, and reports the number removed in the response and the operation log.
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
        // through; the permission gate itself is covered by the auth tests.
        auth_ = std::make_unique<ApiKeyAuth>();
        auth_->set_enabled(false);

        server_ = std::make_unique<httplib::Server>();
        // No sweeper (enqueued is 0 then): the contract under test is the audit's
        // effect on the store and the index, the response and the operation log.
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

    httplib::Result Post(const std::string& body) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        if (body.empty()) return cli.Post(kPath);
        return cli.Post(kPath, body, "application/json");
    }

    std::string LastSummary() const {
        if (oplog_.entries.empty() || !oplog_.entries.back().summary) return "<none>";
        return *oplog_.entries.back().summary;
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

// The default audit removes exactly the excluded entries and says how many, in
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
                                 " enqueued=0");

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
    EXPECT_EQ(LastSummary(), "removed=0 synthesized=0 enqueued=0");
}

// --- #174: a failure after removals reports them ----------------------------------

std::vector<uint64_t> SortedDeleted(cortrix::test::FakeIndex* index) {
    std::vector<uint64_t> d = index->deleted_ids();
    std::sort(d.begin(), d.end());
    return d;
}

// The index delete fails from the second excluded entry: the error says the first
// was removed, the removal is logged with the error's code, and the second entry's
// row stays. A later request then removes what remains and reports only that.
TEST_F(EnrichRoutesTest, AuditFailurePartWayReportsTheRemovalsMadeBeforeIt) {
    sqlite3* db = Db();
    ASSERT_NE(db, nullptr);
    SeedLabelledChunk(db, "doc-skip", 3, 601, R"({"cleaning.skip_index":true})", 7601);
    SeedLabelledChunk(db, "doc-l2", 2, 602, nullptr, 7602);
    harness_->fake_index()->set_mark_delete_fail_from(2);

    auto res = Post("");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 503) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"]["code"], "CX_ERR_PHNSW_NOT_READY");
    EXPECT_EQ(j["error"]["structured_data"]["removed"], 1) << res->body;
    ASSERT_EQ(oplog_.entries.size(), 1u);
    EXPECT_EQ(LastSummary(), "removed=1 failed=CX_ERR_PHNSW_NOT_READY");
    EXPECT_EQ(SortedDeleted(harness_->fake_index()), (std::vector<uint64_t>{7601}));
    EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM contextual_vec_labels WHERE label=7601"), 0);
    EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM contextual_vec_labels WHERE label=7602"), 1);

    harness_->fake_index()->set_mark_delete_fail_from(0);
    auto later = Post("");
    ASSERT_TRUE(later);
    ASSERT_EQ(later->status, 200) << later->body;
    EXPECT_EQ(json::parse(later->body)["removed"], 1);
    EXPECT_EQ(SortedDeleted(harness_->fake_index()), (std::vector<uint64_t>{7601, 7602}));
    EXPECT_EQ(LastSummary().rfind("removed=1 synthesized=", 0), 0u) << LastSummary();
}

// The removals succeed and the synthesis then fails: the error still reports them.
// The synthesis fails deterministically on a dropped ledger table, which the
// reconcile never reads.
TEST_F(EnrichRoutesTest, SynthesisFailureAfterRemovalsReportsThem) {
    sqlite3* db = Db();
    ASSERT_NE(db, nullptr);
    SeedLabelledChunk(db, "doc-skip", 3, 601, R"({"cleaning.skip_index":true})", 7601);
    SeedLabelledChunk(db, "doc-l2", 2, 602, nullptr, 7602);
    Exec(db, "DROP TABLE enrich_state");

    auto res = Post("");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 500) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"]["code"], "CX_ERR_ENRICH_STATE_STORE");
    EXPECT_EQ(j["error"]["structured_data"]["removed"], 2) << res->body;
    EXPECT_EQ(LastSummary(), "removed=2 failed=CX_ERR_ENRICH_STATE_STORE");
    EXPECT_EQ(SortedDeleted(harness_->fake_index()), (std::vector<uint64_t>{7601, 7602}));
    EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM contextual_vec_labels"
                        " WHERE label IN (7601, 7602)"), 0);
}

// A failure before any removal says so with removed = 0, and logs nothing: the log
// records removals, and there were none.
TEST_F(EnrichRoutesTest, AuditFailureBeforeAnyRemovalReportsZeroAndLogsNothing) {
    sqlite3* db = Db();
    ASSERT_NE(db, nullptr);
    SeedLabelledChunk(db, "doc-skip", 3, 601, R"({"cleaning.skip_index":true})", 7601);
    harness_->fake_index()->set_mark_delete_fail_from(1);

    auto res = Post("");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 503) << res->body;
    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"]["code"], "CX_ERR_PHNSW_NOT_READY");
    EXPECT_EQ(j["error"]["structured_data"]["removed"], 0) << res->body;
    EXPECT_TRUE(oplog_.entries.empty());
    EXPECT_TRUE(harness_->fake_index()->deleted_ids().empty());
    EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM contextual_vec_labels WHERE label=7601"), 1);
}

}  // namespace
}  // namespace cortrix
