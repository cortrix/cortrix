#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <thread>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/catalog/gc/gc_thread.h"
#include "cortrix/config/config.h"

// OPEN-2 background GC thread: enabled/disabled gate + that the loop drives the
// manager's three-stage sweep on its interval.
namespace cortrix::catalog::gc {
namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class GcThreadTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }
    void SeedExpiredSoftDelete(const std::string& fh) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO file_locations (file_hash, unit_id, ns_id, tenant_id, size_bytes, "
            "blob_uri, ref_count, first_seen_at, status, deleted_at, created_at) "
            "VALUES (?, 'u1','ns1','t1', 10, 'x/y', 0, 0, 'deleted', ?, 0)";
        ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(), sql, -1, &stmt, nullptr), SQLITE_OK);
        sqlite3_bind_text(stmt, 1, fh.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, NowMs() - 40LL * 24 * 3600 * 1000);  // 40 days ago
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    int FileCount() {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(catalog_.db(),
                  "SELECT COUNT(*) FROM file_locations", -1, &stmt, nullptr), SQLITE_OK);
        int n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return n;
    }
    CatalogDb catalog_;
    NullBlobGcSink sink_;
};

// gc.enabled=false → Start() is a no-op; the thread never launches.
TEST_F(GcThreadTest, DisabledDoesNotStart) {
    GcConfig cfg;
    cfg.enabled = false;
    GcManager mgr(catalog_.db(), &sink_, cfg);
    GcThread thread(&mgr);
    thread.Start();
    EXPECT_FALSE(thread.started());
    thread.Stop();  // safe without a running thread
}

// The loop runs a sweep: a fast test interval drives Stage 2 hard-delete.
TEST_F(GcThreadTest, LoopRunsSweep) {
    SeedExpiredSoftDelete("fh1");
    GcConfig cfg;
    cfg.enabled = true;
    GcManager mgr(catalog_.db(), &sink_, cfg);
    GcThread thread(&mgr);
    thread.set_test_interval_ms(20);
    thread.Start();
    EXPECT_TRUE(thread.started());

    // Wait until the file is collected (bounded poll, no fixed sleep).
    for (int i = 0; i < 100 && FileCount() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    thread.Stop();
    EXPECT_EQ(FileCount(), 0) << "background loop should have hard-deleted the expired file";
}

// Stop() is idempotent and joins cleanly.
TEST_F(GcThreadTest, StopIsIdempotent) {
    GcConfig cfg;
    GcManager mgr(catalog_.db(), &sink_, cfg);
    GcThread thread(&mgr);
    thread.set_test_interval_ms(50);
    thread.Start();
    thread.Stop();
    thread.Stop();
    EXPECT_FALSE(thread.started());
}

}  // namespace
}  // namespace cortrix::catalog::gc
