// Deployment — MetricsServer unit tests (previously ZERO coverage).
// RenderAll multi-source aggregation, AddSource / AddBloomFilterSource (incl.
// null-source ignore), GET /metrics body + content-type, Start/Stop idempotency,
// and the RegisterMetricsServer factory. The DeployMetrics gauges are
// process-global; ResetForTest() keeps the body deterministic across runs.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "httplib.h"

#include "cortrix/deploy/deploy_metrics.h"
#include "cortrix/deploy/metrics_server.h"

namespace cortrix::deploy {
namespace {

// Pick a port unlikely to clash across parallel ctest shards.
int TestPort() { return 19300 + (getpid() % 400); }

class MetricsServerTest : public ::testing::Test {
protected:
    void SetUp() override { DeployMetrics::Instance().ResetForTest(); }
    void TearDown() override { DeployMetrics::Instance().ResetForTest(); }
};

// --- RenderAll: the DeployMetrics gauges are always present (ctor source) ---
TEST_F(MetricsServerTest, RenderAllAlwaysIncludesDeployGauges) {
    MetricsServer srv(TestPort());
    const std::string body = srv.RenderAll();
    // The DeployMetrics::Render() block emits the disk usage gauge name.
    EXPECT_NE(body.find("cortrix_disk_usage_ratio"), std::string::npos);
}

// --- RenderAll concatenates every registered source in order ---
TEST_F(MetricsServerTest, RenderAllAggregatesMultipleSources) {
    MetricsServer srv(TestPort());
    srv.AddSource([] { return std::string("# SOURCE_A\n"); });
    srv.AddSource([] { return std::string("# SOURCE_B\n"); });
    const std::string body = srv.RenderAll();
    const auto a = body.find("# SOURCE_A");
    const auto b = body.find("# SOURCE_B");
    ASSERT_NE(a, std::string::npos);
    ASSERT_NE(b, std::string::npos);
    EXPECT_LT(a, b);  // appended in registration order
}

// --- AddSource ignores a null (empty std::function) source ---
TEST_F(MetricsServerTest, AddSourceIgnoresNull) {
    MetricsServer srv(TestPort());
    const std::string before = srv.RenderAll();
    srv.AddSource(MetricsServer::MetricSource{});  // empty target → ignored
    const std::string after = srv.RenderAll();
    EXPECT_EQ(before, after);  // no crash, no change
}

// --- AddBloomFilterSource renders the four BF gauges ---
TEST_F(MetricsServerTest, AddBloomFilterSourceRendersGauges) {
    MetricsServer srv(TestPort());
    BloomFilterMetricSource bf;
    bf.estimated_count = [] { return uint64_t{1234}; };
    bf.false_positive_rate = [] { return 0.01; };
    bf.last_rebuild_epoch_sec = [] { return int64_t{1700000000}; };
    bf.ready = [] { return true; };
    srv.AddBloomFilterSource(std::move(bf));
    const std::string body = srv.RenderAll();
    EXPECT_NE(body.find("cortrix_bloom_filter_estimated_count"), std::string::npos);
    EXPECT_NE(body.find("cortrix_bloom_filter_ready"), std::string::npos);
    EXPECT_NE(body.find("1234"), std::string::npos);
}

// --- RenderBloomFilterMetrics skips null accessors (free function) ---
TEST_F(MetricsServerTest, RenderBloomFilterMetricsSkipsNullAccessors) {
    BloomFilterMetricSource bf;  // all accessors null
    bf.ready = [] { return false; };
    const std::string body = RenderBloomFilterMetrics(bf);
    // ready accessor present → its gauge renders; estimated_count null → absent.
    EXPECT_NE(body.find("cortrix_bloom_filter_ready"), std::string::npos);
    EXPECT_EQ(body.find("cortrix_bloom_filter_estimated_count"), std::string::npos);
}

// --- Start binds + serves GET /metrics with the OpenMetrics content type ---
TEST_F(MetricsServerTest, StartServesMetricsRoute) {
    MetricsServer srv(TestPort());
    srv.AddSource([] { return std::string("# CUSTOM_PROBE 1\n"); });
    ASSERT_TRUE(srv.Start());

    httplib::Client cli("127.0.0.1", srv.port());
    httplib::Result res;
    for (int i = 0; i < 50; ++i) {
        res = cli.Get("/metrics");
        if (res) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->get_header_value("Content-Type"), MetricsServer::kContentType);
    EXPECT_NE(res->body.find("# CUSTOM_PROBE 1"), std::string::npos);
    EXPECT_NE(res->body.find("cortrix_disk_usage_ratio"), std::string::npos);
    srv.Stop();
}

// --- Start is idempotent (second call is a no-op success on the same port) ---
TEST_F(MetricsServerTest, StartIsIdempotent) {
    MetricsServer srv(TestPort());
    ASSERT_TRUE(srv.Start());
    EXPECT_TRUE(srv.Start());  // already running → returns true, does not re-bind
    srv.Stop();
}

// --- Stop is idempotent (and safe to call when never started) ---
TEST_F(MetricsServerTest, StopIsIdempotent) {
    MetricsServer srv(TestPort());
    srv.Stop();  // never started → no-op
    ASSERT_TRUE(srv.Start());
    srv.Stop();
    srv.Stop();  // double stop → no crash
}

// NOTE: a "Start fails on port clash" test was intentionally omitted. cpp-httplib
// sets SO_REUSEPORT on the listen socket on macOS/Linux, so a second bind to the
// same port SUCCEEDS rather than failing — port-clash is not observable here.

// --- port() reflects the constructor argument ---
TEST_F(MetricsServerTest, PortAccessorReflectsCtor) {
    MetricsServer srv(19577);
    EXPECT_EQ(srv.port(), 19577);
}

// --- RegisterMetricsServer factory: builds + starts, BF source bound when set ---
TEST_F(MetricsServerTest, RegisterMetricsServerFactoryStartsAndBindsBf) {
    const int port = TestPort() + 1;
    BloomFilterMetricSource bf;
    bf.ready = [] { return true; };
    auto srv = RegisterMetricsServer(port, std::move(bf));
    ASSERT_NE(srv, nullptr);
    EXPECT_EQ(srv->port(), port);

    httplib::Client cli("127.0.0.1", port);
    httplib::Result res;
    for (int i = 0; i < 50; ++i) {
        res = cli.Get("/metrics");
        if (res) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("cortrix_bloom_filter_ready"), std::string::npos);
    srv->Stop();
}

// --- RegisterMetricsServer with an all-null BF source omits the BF gauges ---
TEST_F(MetricsServerTest, RegisterMetricsServerNoBfSourceOmitsBfGauges) {
    const int port = TestPort() + 2;
    auto srv = RegisterMetricsServer(port, BloomFilterMetricSource{});
    ASSERT_NE(srv, nullptr);
    const std::string body = srv->RenderAll();
    EXPECT_EQ(body.find("cortrix_bloom_filter_ready"), std::string::npos);
    srv->Stop();
}

// NOTE: a "factory returns nullptr on port clash" test was intentionally omitted
// for the same SO_REUSEPORT reason as above — the second bind succeeds, so the
// factory does not observe a clash.

}  // namespace
}  // namespace cortrix::deploy
