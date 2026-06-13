#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/agent_trace/agent_trace_writer_impl.h"
#include "cortrix/agent_trace/f13_cleanup_registrar.h"
#include "cortrix/agent_trace/interaction_sources_schema.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/observability/cleanup_scheduler.h"

// S10 + S11(cleanup) coverage: F13 registers agent_trace (90d) + interaction_log
// (180d) on the shared F18a CleanupScheduler; RunCleanupNow() prunes both;
// interaction_sources cascades via FK; the ISO-8601 cutoff matches the real
// interaction_log.created_at TEXT column.
namespace cortrix::agent_trace {
namespace {

class StubGlobalConfig : public cortrix::IGlobalConfig {
public:
    Result<std::string> GetString(const std::string&) const override { return Status::InvalidArgument("x"); }
    Result<bool> GetBool(const std::string&) const override { return Status::InvalidArgument("x"); }
    Result<int> GetInt(const std::string&) const override { return Status::InvalidArgument("x"); }
    Result<float> GetFloat(const std::string&) const override { return Status::InvalidArgument("x"); }
    void OnChange(std::function<void(const std::string&)>) override {}
};

void Exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);
}

int64_t QueryInt(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK) << sqlite3_errmsg(db);
    int64_t v = -1;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

class F13CleanupRegistrarTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        Exec(db_, "PRAGMA foreign_keys=ON;");  // exercise the interaction_sources cascade
        cortrix::catalog::SchemaMigrator m;
        m.Register(&at_provider_);
        m.Register(&src_provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        Exec(db_, R"(
            CREATE TABLE interaction_log (
                id TEXT PRIMARY KEY, session_id TEXT NOT NULL, namespace_name TEXT NOT NULL,
                user_id TEXT, role TEXT NOT NULL, content TEXT NOT NULL, query_type TEXT,
                status TEXT, latency_ms INTEGER DEFAULT 0, metadata_json TEXT,
                created_at TEXT NOT NULL
            );)");
        config_ = std::make_shared<StubGlobalConfig>();
        writer_ = std::make_shared<AgentTraceWriterImpl>(db_, config_);
        registrar_ = std::make_unique<F13CleanupRegistrar>(writer_, db_, config_);
    }
    void TearDown() override {
        registrar_.reset();
        writer_.reset();
        if (db_) sqlite3_close(db_);
    }

    AgentTraceSchemaProvider at_provider_;
    InteractionSourcesSchemaProvider src_provider_;
    std::shared_ptr<StubGlobalConfig> config_;
    std::shared_ptr<AgentTraceWriterImpl> writer_;
    std::unique_ptr<F13CleanupRegistrar> registrar_;
    sqlite3* db_ = nullptr;
};

TEST_F(F13CleanupRegistrarTest, RegistersBothTables) {
    observability::CleanupScheduler sched;
    registrar_->Register(sched);
    EXPECT_EQ(sched.registered_count(), 2u);
}

TEST_F(F13CleanupRegistrarTest, RunCleanupNowPrunesBoth) {
    const int64_t now = NowMs();

    // agent_trace: one 91-day-old + one fresh.
    AgentTraceEntry old_at;
    old_at.session_id = "s"; old_at.method = "a"; old_at.source = "mcp";
    old_at.created_at = now - 91LL * 86400000LL;
    writer_->Write(old_at);
    AgentTraceEntry new_at;
    new_at.session_id = "s"; new_at.method = "b"; new_at.source = "mcp";
    new_at.created_at = now - 1000;
    writer_->Write(new_at);

    // interaction_log: one 181-day-old + one fresh (created_at ISO-8601 TEXT).
    const std::string old_iso = F13CleanupRegistrar::FormatIso8601Utc(now - 181LL * 86400000LL);
    const std::string new_iso = F13CleanupRegistrar::FormatIso8601Utc(now - 1000);
    Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content, created_at) "
              "VALUES('old','s','sales','alice','user','q','" + old_iso + "');");
    Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content, created_at) "
              "VALUES('new','s','sales','alice','user','q','" + new_iso + "');");
    // a source row on the old interaction (must cascade-delete).
    Exec(db_, "INSERT INTO interaction_sources(interaction_id, source_block_id, source_type, relevance_score, snippet) "
              "VALUES('old','b1','block',0.5,'x');");

    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace"), 2);
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM interaction_log"), 2);
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM interaction_sources"), 1);

    observability::CleanupScheduler sched;
    registrar_->Register(sched);
    sched.RunCleanupNow();

    // agent_trace: 91d row gone, fresh kept.
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace"), 1);
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace WHERE method='b'"), 1);
    // interaction_log: 181d row gone, fresh kept.
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM interaction_log"), 1);
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM interaction_log WHERE id='new'"), 1);
    // interaction_sources cascaded with the deleted interaction.
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM interaction_sources"), 0);
}

TEST_F(F13CleanupRegistrarTest, CleanupInteractionLogReturnsDeletedCount) {
    const int64_t now = NowMs();
    const std::string old_iso = F13CleanupRegistrar::FormatIso8601Utc(now - 200LL * 86400000LL);
    Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content, created_at) "
              "VALUES('o1','s','sales','a','user','q','" + old_iso + "');");
    Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content, created_at) "
              "VALUES('o2','s','sales','a','user','q','" + old_iso + "');");
    EXPECT_EQ(registrar_->CleanupInteractionLog(), 2);
}

TEST_F(F13CleanupRegistrarTest, FormatIso8601UtcShape) {
    // 2026-06-01T00:00:00.000Z is a known epoch-ms; just assert the shape + 'Z'.
    std::string s = F13CleanupRegistrar::FormatIso8601Utc(1764547200123LL);
    EXPECT_EQ(s.size(), 24u);                 // YYYY-MM-DDTHH:MM:SS.mmmZ
    EXPECT_EQ(s.back(), 'Z');
    EXPECT_EQ(s[4], '-');
    EXPECT_EQ(s[10], 'T');
    EXPECT_EQ(s[19], '.');
}

TEST_F(F13CleanupRegistrarTest, NullDbInteractionLogCleanupIsZero) {
    F13CleanupRegistrar no_db(writer_, nullptr, config_);
    EXPECT_EQ(no_db.CleanupInteractionLog(), 0);
}

}  // namespace
}  // namespace cortrix::agent_trace
