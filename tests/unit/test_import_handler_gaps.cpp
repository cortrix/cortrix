// import_handler.cpp gap coverage. Complements test_import_manager.cpp (which owns
// the ImportHandlerTest suite) and test_errpath_import.cpp (ImportHandlerErrPathTest).
// This file's suite names are module-prefixed (ImportHandlerGaps) for global gtest
// uniqueness.
//
// Targets the import_handler.cpp lines the existing suites do not reach:
//   * ParseImportRequest:  text_strategy-not-string (67), valid "merge" strategy
//     (74-75), merge_size int (77-78).
//   * HandleStartImport:   the parse-failure early-return arm (106-107).
//   * HandleRegisterConnection: the expire_days int branch (202-203) and the
//     Register-returns-error arm (207-208).
//   * HandleRevokeConnection:   the no-"reason" ternary false branch (247).
//   * CodeFromStatus:      the unrecognized-prefix -> kConnectionFailed fallback (29),
//     reached via a connection-manager Status whose message carries no CX_ERR_IMPORT_*
//     token (HttpFor / ErrorBody route through CodeFromStatus).
//
// Deterministic: no socket, no network, no LLM. The handler methods are called
// directly; a fake IConnectionManager supplies the error / non-F16a-token arms.
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_context.h"
#include "cortrix/import/connection_manager.h"
#include "cortrix/import/import_manager.h"
#include "cortrix/import/import_task_queue.h"
#include "cortrix/import/query_executor.h"
#include "cortrix/import/spc_feed.h"
#include "cortrix/import/text_serializer.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/server/import_handler.h"

namespace cortrix::server {
namespace {

using cortrix::import::ConnectionInfo;
using cortrix::import::ConnectionManager;
using cortrix::import::ConnectionRefId;
using cortrix::import::IConnectionManager;
using cortrix::import::ImportManager;
using cortrix::import::ImportManagerConfig;
using cortrix::import::InMemoryBlockCleaner;
using cortrix::import::InMemoryConnectionStore;
using cortrix::import::InMemorySecretStore;
using cortrix::import::InMemorySpcFeeder;
using cortrix::import::InMemoryImportTaskStore;
using cortrix::import::TenantId;
using cortrix::import::TextSerializer;

AuthContext AdminCtx(const std::string& tenant = "t1") {
    AuthContext c;
    c.tenant_id = tenant;
    c.user_id = "admin1";
    c.permissions = kPermAdmin | kPermRead | kPermWrite;
    return c;
}

// A real QueryExecutor stand-in: returns canned rows so StartImport's pre-check
// runs without a live PG. Estimate succeeds; Execute returns no rows.
class StubQueryExecutor : public cortrix::import::IQueryExecutor {
public:
    Result<int64_t> EstimateRowCount(const std::string&,
                                     const cortrix::import::QueryRequest&,
                                     const cortrix::import::QueryConstraints&) override {
        return static_cast<int64_t>(0);
    }
    Result<std::vector<cortrix::import::DbRow>> Execute(
        const std::string&, const cortrix::import::QueryRequest&,
        const cortrix::import::QueryConstraints&) override {
        return std::vector<cortrix::import::DbRow>{};
    }
};

// A fake IConnectionManager whose Register / Revoke return a Status whose message
// carries NO CX_ERR_IMPORT_* token. This drives both the Register/Revoke error arms
// AND CodeFromStatus's unrecognized-prefix fallback (kConnectionFailed).
class FailingConnMgr : public IConnectionManager {
public:
    Result<ConnectionRefId> Register(const std::string&, const std::string&, const TenantId&,
                                     const std::string&, std::optional<int>) override {
        // Plain message, no CX_ERR_IMPORT_ prefix -> CodeFromStatus falls back.
        return Status::Internal("secret store write failed");
    }
    Result<std::string> ResolveDsn(const ConnectionRefId&, const AuthContext&) override {
        return Status::Internal("unused");
    }
    std::vector<ConnectionInfo> ListConnections(const TenantId&) override { return {}; }
    Status Revoke(const ConnectionRefId&, const AuthContext&, const std::string&) override {
        return Status::Internal("revoke backend unavailable");  // no CX token
    }
};

// Builds a real ImportManager over a fake conn manager + stub executor so the
// handler can be constructed for the register/revoke arms (those paths never touch
// the import manager itself).
std::shared_ptr<ImportManager> MakeMgr(std::shared_ptr<IConnectionManager> conn_mgr) {
    ImportManagerConfig cfg;
    cfg.pg_host = "db.internal";
    cfg.pg_db = "crm";
    return std::make_shared<ImportManager>(
        conn_mgr, std::make_shared<StubQueryExecutor>(), std::make_shared<TextSerializer>(),
        std::make_shared<InMemorySpcFeeder>(), std::make_shared<InMemoryBlockCleaner>(),
        std::make_shared<InMemoryImportTaskStore>(),
        std::shared_ptr<observability::IOperationLogger>(nullptr), cfg);
}

// ---------------------------------------------------------------------------
// ParseImportRequest: text_strategy + merge_size branches.
// ---------------------------------------------------------------------------

// text_strategy present but not a string -> CX_ERR_IMPORT_INVALID_SQL (line 67).
TEST(ImportHandlerGaps, TextStrategyNotStringIsInvalid) {
    nlohmann::json body = {{"namespace", "ns"}, {"connection_ref", "r"},
                           {"table", "users"}, {"text_strategy", 123}};
    auto r = ImportHandler::ParseImportRequest(body);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("text_strategy must be a string"), std::string::npos)
        << r.status().message();
}

// A VALID non-default strategy ("merge") is accepted and stored (lines 69-75).
TEST(ImportHandlerGaps, MergeStrategyAccepted) {
    nlohmann::json body = {{"namespace", "ns"}, {"connection_ref", "r"},
                           {"table", "users"}, {"text_strategy", "merge"}};
    auto r = ImportHandler::ParseImportRequest(body);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().text_strategy, cortrix::import::TextStrategy::kMerge);
}

// merge_size as an integer is read into the request (lines 76-78).
TEST(ImportHandlerGaps, MergeSizeIntegerStored) {
    nlohmann::json body = {{"namespace", "ns"}, {"connection_ref", "r"},
                           {"table", "users"}, {"text_strategy", "merge"}, {"merge_size", 7}};
    auto r = ImportHandler::ParseImportRequest(body);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().merge_size, 7);
}

// ---------------------------------------------------------------------------
// HandleStartImport: the parse-failure early-return arm (105-108).
// ---------------------------------------------------------------------------

// A body that fails ParseImportRequest (missing namespace) returns through the
// out_http_status = HttpFor(parsed.status()) / ErrorBody(parsed.status()) arm -- a
// 400 INVALID_SQL envelope, never reaching the import manager.
TEST(ImportHandlerGaps, StartImportParseFailureReturns400) {
    auto conn_mgr = std::make_shared<FailingConnMgr>();
    auto handler = ImportHandler(MakeMgr(conn_mgr), conn_mgr);
    nlohmann::json body = {{"connection_ref", "r"}, {"table", "users"}};  // no namespace
    int http = 0;
    auto out = handler.HandleStartImport(body, AdminCtx(), http);
    EXPECT_EQ(http, 400) << out.dump();
    ASSERT_TRUE(out.contains("error"));
    EXPECT_EQ(out["error"]["code"], "CX_ERR_IMPORT_INVALID_SQL") << out.dump();
}

// ---------------------------------------------------------------------------
// HandleRegisterConnection: expire_days int (200-203) + Register error (206-209).
// ---------------------------------------------------------------------------

// expire_days as an integer is forwarded; the (failing) Register then surfaces an
// error envelope -- this hits BOTH the expire_days branch and the Register-error arm.
TEST(ImportHandlerGaps, RegisterWithExpireDaysAndBackendError) {
    auto conn_mgr = std::make_shared<FailingConnMgr>();
    auto handler = ImportHandler(MakeMgr(conn_mgr), conn_mgr);
    nlohmann::json body = {{"name", "crm"}, {"dsn", "postgres://u:p@h:5432/db"},
                           {"expire_days", 30}};
    int http = 0;
    auto out = handler.HandleRegisterConnection(body, AdminCtx(), http);
    // FailingConnMgr::Register returns a non-import Status -> CodeFromStatus falls back
    // to kConnectionFailed (line 29) -> HttpFor maps to its http_status (503).
    EXPECT_GE(http, 400) << out.dump();
    ASSERT_TRUE(out.contains("error"));
    EXPECT_TRUE(out["error"].contains("code")) << out.dump();
}

// A successful register WITH expire_days against the real ConnectionManager -- pins
// the expire_days branch (202-203) on the success path so the optional is actually
// consumed (not just on the error arm above).
TEST(ImportHandlerGaps, RegisterWithExpireDaysSucceeds) {
    auto secret = std::make_shared<InMemorySecretStore>();
    auto conn_store = std::make_shared<InMemoryConnectionStore>();
    auto conn_mgr = std::make_shared<ConnectionManager>(secret, conn_store);
    auto handler = ImportHandler(MakeMgr(conn_mgr), conn_mgr);
    nlohmann::json body = {{"name", "crm"}, {"dsn", "postgres://u:p@h:5432/db"},
                           {"expire_days", 45}};
    int http = 0;
    auto out = handler.HandleRegisterConnection(body, AdminCtx(), http);
    EXPECT_EQ(http, 200) << out.dump();
    EXPECT_TRUE(out.contains("ref_id"));
}

// ---------------------------------------------------------------------------
// HandleRevokeConnection: the no-"reason" ternary false branch (245-247) + the
// non-import Revoke error -> CodeFromStatus fallback (29).
// ---------------------------------------------------------------------------

// Revoke called with NO "reason" field -> reason defaults to "" (line 247 false arm),
// and the failing conn manager returns a non-import Status -> fallback code path.
TEST(ImportHandlerGaps, RevokeWithoutReasonAndBackendError) {
    auto conn_mgr = std::make_shared<FailingConnMgr>();
    auto handler = ImportHandler(MakeMgr(conn_mgr), conn_mgr);
    int http = 0;
    auto out = handler.HandleRevokeConnection("ref-x", nlohmann::json::object(), AdminCtx(), http);
    EXPECT_GE(http, 400) << out.dump();
    ASSERT_TRUE(out.contains("error"));
    EXPECT_TRUE(out["error"].contains("code")) << out.dump();
}

// Revoke with a non-object body -> reason stays "" (the is_object() guard's false arm
// of line 245-247) and still routes to the conn manager.
TEST(ImportHandlerGaps, RevokeWithNonObjectBody) {
    auto conn_mgr = std::make_shared<FailingConnMgr>();
    auto handler = ImportHandler(MakeMgr(conn_mgr), conn_mgr);
    int http = 0;
    auto out = handler.HandleRevokeConnection("ref-y", nlohmann::json::array(), AdminCtx(), http);
    EXPECT_GE(http, 400) << out.dump();
    ASSERT_TRUE(out.contains("error"));
}

}  // namespace
}  // namespace cortrix::server
