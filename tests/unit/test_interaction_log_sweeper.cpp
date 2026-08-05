#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/agent_trace/f13_cleanup_registrar.h"  // FormatIso8601Utc
#include "cortrix/agent_trace/interaction_log_sweeper.h"
#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/resource/namespace_facade.h"

#include "ns_pool_test_helper.h"

// [agent trace TC4] InteractionLogSweeper: the per-NS interaction_log 180d cleanup. It
// iterates namespaces (via INSRouter), acquires each memory.db façade, and deletes
// interaction_log rows older than the retention window from EACH namespace (the
// global agent_trace cleanup is the single-db F13CleanupRegistrar path; interaction_log
// is per-NS so a single-db registrar cannot reach the N tables).
namespace cortrix::agent_trace {
namespace {

using ::testing::_;
using ::testing::Invoke;
namespace fs = std::filesystem;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

void Exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "");
    sqlite3_free(err);
}

int64_t QueryInt(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    int64_t v = -1;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

// A 180d retention stub (matches the canonical CE default).
class StubGlobalConfig : public cortrix::IGlobalConfig {
public:
    Result<std::string> GetString(const std::string&) const override { return Status::InvalidArgument("x"); }
    Result<bool> GetBool(const std::string&) const override { return Status::InvalidArgument("x"); }
    Result<int> GetInt(const std::string&) const override { return Status::InvalidArgument("x"); }
    Result<float> GetFloat(const std::string&) const override { return Status::InvalidArgument("x"); }
    void OnChange(std::function<void(const std::string&)>) override {}
};

catalog::BatchResult<std::string> NsList(std::vector<std::string> names) {
    catalog::BatchResult<std::string> r;
    r.results = std::move(names);
    return r;
}

class InteractionLogSweeperTest : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
            fs::temp_directory_path() /
            ("ilsweep_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        ASSERT_TRUE(harness_->Admit(kNsA).ok());
        ASSERT_TRUE(harness_->Admit(kNsB).ok());
        config_ = std::make_shared<StubGlobalConfig>();

        ON_CALL(router_, ListNamespaces(_))
            .WillByDefault(Invoke([](const catalog::ListNamespacesOptions&) {
                return cortrix::Result<catalog::BatchResult<std::string>>(
                    NsList({kNsA, kNsB}));
            }));
    }

    // Insert an interaction_log row (with a chosen ISO-8601 created_at) into one NS's
    // memory.db. interaction_log + interaction_sources are created on memory.db init.
    void SeedInteraction(const std::string& ns, const std::string& id,
                         int64_t created_ms, bool with_source) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), ns);
        ASSERT_TRUE(facade.Acquire().ok());
        sqlite3* db = facade.memory().db_handle();
        const std::string iso = F13CleanupRegistrar::FormatIso8601Utc(created_ms);
        Exec(db, "INSERT INTO interaction_log"
                 "(id, session_id, namespace_name, user_id, role, content, created_at) "
                 "VALUES('" + id + "','s','" + ns + "','alice','user','q','" + iso + "');");
        if (with_source) {
            Exec(db, "INSERT INTO interaction_sources"
                     "(interaction_id, source_block_id, source_type, relevance_score, snippet) "
                     "VALUES('" + id + "','b1','block',0.5,'x');");
        }
    }

    int64_t CountInteractions(const std::string& ns) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), ns);
        EXPECT_TRUE(facade.Acquire().ok());
        return QueryInt(facade.memory().db_handle(), "SELECT COUNT(*) FROM interaction_log");
    }
    int64_t CountSources(const std::string& ns) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), ns);
        EXPECT_TRUE(facade.Acquire().ok());
        return QueryInt(facade.memory().db_handle(), "SELECT COUNT(*) FROM interaction_sources");
    }
    int64_t SurvivorIs(const std::string& ns, const std::string& id) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), ns);
        EXPECT_TRUE(facade.Acquire().ok());
        return QueryInt(facade.memory().db_handle(),
                        "SELECT COUNT(*) FROM interaction_log WHERE id='" + id + "'");
    }

    static constexpr const char* kNsA = "sales";
    static constexpr const char* kNsB = "support";
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> router_;
    std::shared_ptr<StubGlobalConfig> config_;
};

// Expired interaction_log rows are deleted across EVERY namespace; fresh rows stay;
// interaction_sources cascades with the deleted interaction (FK ON DELETE CASCADE).
TEST_F(InteractionLogSweeperTest, SweepsAllNamespacesAndCascades) {
    const int64_t now = NowMs();
    const int64_t old_ms = now - 181LL * 86400000LL;  // past 180d
    const int64_t new_ms = now - 1000;

    SeedInteraction(kNsA, "a_old", old_ms, /*with_source=*/true);
    SeedInteraction(kNsA, "a_new", new_ms, false);
    SeedInteraction(kNsB, "b_old", old_ms, false);

    ASSERT_EQ(CountInteractions(kNsA), 2);
    ASSERT_EQ(CountInteractions(kNsB), 1);
    ASSERT_EQ(CountSources(kNsA), 1);

    InteractionLogSweeper sweeper(&router_, &harness_->ipool(), config_);
    auto r = sweeper.RunOnce();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().namespaces_swept, 2);
    EXPECT_EQ(r.value().namespaces_skipped, 0);
    EXPECT_EQ(r.value().interactions_deleted, 2);  // a_old + b_old

    EXPECT_EQ(CountInteractions(kNsA), 1);  // a_new kept
    EXPECT_EQ(SurvivorIs(kNsA, "a_new"), 1);
    EXPECT_EQ(CountInteractions(kNsB), 0);  // b_old gone
    EXPECT_EQ(CountSources(kNsA), 0);       // cascaded with a_old
}

// A namespace that cannot be acquired is skipped (counted), not fatal.
TEST_F(InteractionLogSweeperTest, SkipsUnacquirableNamespace) {
    // Router lists a third NS that was never Admit()'d, so its façade.Acquire() fails.
    ON_CALL(router_, ListNamespaces(_))
        .WillByDefault(Invoke([](const catalog::ListNamespacesOptions&) {
            return cortrix::Result<catalog::BatchResult<std::string>>(
                NsList({kNsA, "ghost"}));
        }));
    SeedInteraction(kNsA, "a_old", NowMs() - 181LL * 86400000LL, false);

    InteractionLogSweeper sweeper(&router_, &harness_->ipool(), config_);
    auto r = sweeper.RunOnce();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().namespaces_swept, 1);
    EXPECT_EQ(r.value().namespaces_skipped, 1);
    EXPECT_EQ(r.value().interactions_deleted, 1);
}

// Enumeration failure propagates (cannot list namespaces -> error, not silent no-op).
TEST_F(InteractionLogSweeperTest, PropagatesEnumerationError) {
    ON_CALL(router_, ListNamespaces(_))
        .WillByDefault(Invoke([](const catalog::ListNamespacesOptions&) {
            return cortrix::Result<catalog::BatchResult<std::string>>(
                Status::Internal("router down"));
        }));
    InteractionLogSweeper sweeper(&router_, &harness_->ipool(), config_);
    auto r = sweeper.RunOnce();
    EXPECT_FALSE(r.ok());
}

}  // namespace
}  // namespace cortrix::agent_trace
