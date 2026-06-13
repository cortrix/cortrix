#include <benchmark/benchmark.h>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include "httplib.h"
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// HTTP benchmark environment: lightweight httplib server simulating Cortrix API
// ---------------------------------------------------------------------------

struct HttpEnv {
    httplib::Server svr;
    std::thread server_thread;
    int port = 0;

    HttpEnv() {
        // Register minimal routes matching Cortrix API
        svr.Get("/api/v1/health", [](const httplib::Request&, httplib::Response& res) {
            nlohmann::json body = {
                {"status", "healthy"},
                {"uptime_seconds", 42}
            };
            res.set_content(body.dump(), "application/json");
        });

        svr.Get("/api/v1/namespaces", [](const httplib::Request&, httplib::Response& res) {
            nlohmann::json body = {
                {"namespaces", nlohmann::json::array({
                    {{"name", "default"}, {"doc_count", 0}}
                })}
            };
            res.set_content(body.dump(), "application/json");
        });

        port = svr.bind_to_any_port("127.0.0.1");
        server_thread = std::thread([this]() {
            svr.listen_after_bind();
        });

        // Wait for server readiness
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~HttpEnv() {
        svr.stop();
        if (server_thread.joinable()) server_thread.join();
    }
};

// ---------------------------------------------------------------------------
// BM_HealthEndpoint – GET /api/v1/health
// ---------------------------------------------------------------------------

static void BM_HealthEndpoint(benchmark::State& state) {
    HttpEnv env;
    httplib::Client cli("127.0.0.1", env.port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);

    for (auto _ : state) {
        auto res = cli.Get("/api/v1/health");
        benchmark::DoNotOptimize(res);
    }
}
BENCHMARK(BM_HealthEndpoint);

// ---------------------------------------------------------------------------
// BM_NamespaceList – GET /api/v1/namespaces
// ---------------------------------------------------------------------------

static void BM_NamespaceList(benchmark::State& state) {
    HttpEnv env;
    httplib::Client cli("127.0.0.1", env.port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);

    for (auto _ : state) {
        auto res = cli.Get("/api/v1/namespaces");
        benchmark::DoNotOptimize(res);
    }
}
BENCHMARK(BM_NamespaceList);

// ---------------------------------------------------------------------------
// BM_HealthEndpointLatency – measure and report P50/P99
// ---------------------------------------------------------------------------

static void BM_HealthEndpointLatency(benchmark::State& state) {
    HttpEnv env;
    httplib::Client cli("127.0.0.1", env.port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);

    std::vector<double> latencies;
    latencies.reserve(10000);

    for (auto _ : state) {
        auto start = std::chrono::high_resolution_clock::now();
        auto res = cli.Get("/api/v1/health");
        auto end = std::chrono::high_resolution_clock::now();
        benchmark::DoNotOptimize(res);

        double us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(us);
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        size_t n = latencies.size();
        state.counters["p50_us"] = latencies[n * 50 / 100];
        state.counters["p99_us"] = latencies[n * 99 / 100];
        state.counters["max_us"] = latencies.back();
    }
}
BENCHMARK(BM_HealthEndpointLatency)->Iterations(1000);
