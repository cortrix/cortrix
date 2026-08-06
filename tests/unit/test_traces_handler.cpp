#include <gtest/gtest.h>

#include <sqlite3.h>

#include <functional>
#include <memory>
#include <string>

#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/agent_trace/agent_trace_writer_impl.h"
#include "cortrix/agent_trace/traces_handler.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/i_global_config.h"

// S4 coverage: GET /traces/{session_id} permission + pagination wiring —
// owner resolved via interaction_log, non-admin cross-user -> UNAUTHORIZED, admin
// nonexistent -> SESSION_NOT_FOUND, owner sees own (incl. empty), invalid filter
// token propagated, and the 1MB soft-limit warning.
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

class TracesHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        AgentTraceMetrics::Instance().ResetForTest();
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());
        // interaction_log for session->owner resolution (MVP shape).
        Exec(db_, R"(
            CREATE TABLE interaction_log (
                id TEXT PRIMARY KEY, session_id TEXT NOT NULL, namespace_name TEXT NOT NULL,
                user_id TEXT, role TEXT NOT NULL, content TEXT NOT NULL, query_type TEXT,
                status TEXT, latency_ms INTEGER DEFAULT 0, metadata_json TEXT,
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
            );)");
        config_ = std::make_shared<StubGlobalConfig>();
        writer_ = std::make_shared<AgentTraceWriterImpl>(db_, config_);
        handler_ = std::make_unique<TracesHandler>(writer_, db_);

        // Session "alice-sess" owned by alice, with 3 traces.
        Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content) "
                  "VALUES('i1','alice-sess','sales','alice','user','q');");
        for (int i = 0; i < 3; ++i) {
            AgentTraceEntry e;
            e.session_id = "alice-sess";
            e.method = "m" + std::to_string(i);
            e.source = "mcp";
            e.created_at = 1000 + i;
            writer_->Write(e);
        }
    }
    void TearDown() override {
        handler_.reset();
        writer_.reset();
        if (db_) sqlite3_close(db_);
        AgentTraceMetrics::Instance().ResetForTest();
    }

    AgentTraceSchemaProvider provider_;
    std::shared_ptr<StubGlobalConfig> config_;
    std::shared_ptr<AgentTraceWriterImpl> writer_;
    std::unique_ptr<TracesHandler> handler_;
    sqlite3* db_ = nullptr;
};

TEST_F(TracesHandlerTest, OwnerReadsOwnSession) {
    auto r = handler_->GetSession("alice-sess", TraceFilter{}, RequesterContext{"alice", false});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().session.trace_count, 3);
    EXPECT_FALSE(r.value().response_size_warning);
    EXPECT_EQ(AgentTraceMetrics::Instance().TracesQueryCount(
                  AgentTraceMetrics::Role::kUser, AgentTraceMetrics::Endpoint::kTraces), 1u);
}

TEST_F(TracesHandlerTest, NonAdminCrossUserIsUnauthorized) {
    auto r = handler_->GetSession("alice-sess", TraceFilter{}, RequesterContext{"mallory", false});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kPermissionDenied);
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_UNAUTHORIZED"), std::string::npos);
}

TEST_F(TracesHandlerTest, NonAdminUnknownSessionIsUnauthorizedNotLeaked) {
    // Session with no interaction_log owner row -> unknown owner -> a non-admin
    // gets UNAUTHORIZED (we do not reveal nonexistence).
    auto r = handler_->GetSession("ghost-sess", TraceFilter{}, RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_UNAUTHORIZED"), std::string::npos);
}

TEST_F(TracesHandlerTest, AdminReadsAnyOwnedSession) {
    //: admin reading alice's session emits the forensics structured log.
    testing::internal::CaptureStderr();
    auto r = handler_->GetSession("alice-sess", TraceFilter{}, RequesterContext{"root", true});
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().session.trace_count, 3);
    EXPECT_EQ(AgentTraceMetrics::Instance().TracesQueryCount(
                  AgentTraceMetrics::Role::kAdmin, AgentTraceMetrics::Endpoint::kTraces), 1u);
    // forensics line present (admin != owner alice).
    EXPECT_NE(err.find("admin_cross_user_access"), std::string::npos);
    EXPECT_NE(err.find("\"target_user_id\":\"alice\""), std::string::npos);
}

// An admin reading their OWN session does NOT emit a cross-user forensics line.
TEST_F(TracesHandlerTest, AdminReadingOwnSessionNoForensics) {
    Exec(db_, "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content) "
              "VALUES('i-root','root-sess','sales','root','user','q');");
    AgentTraceEntry e;
    e.session_id = "root-sess"; e.method = "m"; e.source = "mcp"; e.created_at = 1;
    writer_->Write(e);
    testing::internal::CaptureStderr();
    auto r = handler_->GetSession("root-sess", TraceFilter{}, RequesterContext{"root", true});
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(err.find("admin_cross_user_access"), std::string::npos);
}

TEST_F(TracesHandlerTest, AdminNonexistentSessionIsNotFound) {
    auto r = handler_->GetSession("ghost-sess", TraceFilter{}, RequesterContext{"root", true});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_SESSION_NOT_FOUND"), std::string::npos);
}

// A session that exists in agent_trace but has no interaction_log owner: admin can
// still read it (traces present -> not "not found"); the owner lookup being absent
// does not block admin.
TEST_F(TracesHandlerTest, AdminReadsOwnerlessSessionWithTraces) {
    AgentTraceEntry e;
    e.session_id = "orphan-sess";
    e.method = "x";
    e.source = "mcp";
    e.created_at = 2000;
    writer_->Write(e);
    auto r = handler_->GetSession("orphan-sess", TraceFilter{}, RequesterContext{"root", true});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().session.trace_count, 1);
}

TEST_F(TracesHandlerTest, InvalidFilterTokenPropagated) {
    TraceFilter bad;
    bad.limit = 0;
    auto r = handler_->GetSession("alice-sess", bad, RequesterContext{"alice", false});
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_TRACE_INVALID_FILTER"), std::string::npos);
}

TEST_F(TracesHandlerTest, ResponseSizeWarningOnLargePage) {
    // One trace with a ~1.1MB params payload trips the 1MB soft limit.
    AgentTraceEntry big;
    big.session_id = "alice-sess";
    big.method = "huge";
    big.source = "mcp";
    big.created_at = 2000;
    big.params = std::string(1100 * 1024, 'x');
    writer_->Write(big);

    auto r = handler_->GetSession("alice-sess", TraceFilter{}, RequesterContext{"alice", false});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().response_size_warning);
}

// A handler built without a DB handle treats all ownership as unknown: non-admin
// is always UNAUTHORIZED, admin always works (no owner lookup).
TEST_F(TracesHandlerTest, NullDbHandleAdminOnly) {
    TracesHandler no_db(writer_, nullptr);
    EXPECT_FALSE(no_db.GetSession("alice-sess", TraceFilter{}, RequesterContext{"alice", false}).ok());
    auto r = no_db.GetSession("alice-sess", TraceFilter{}, RequesterContext{"root", true});
    EXPECT_TRUE(r.ok());
}

}  // namespace
}  // namespace cortrix::agent_trace
