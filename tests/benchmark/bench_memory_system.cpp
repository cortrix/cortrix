#include <benchmark/benchmark.h>
#include <filesystem>
#include <string>
#include <vector>
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_injector.h"
#include "cortrix/memory/interaction_log.h"
#include "cortrix/memory/memory_session.h"
#include "cortrix/config/config.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static fs::path MakeTempDir(const std::string& label) {
    auto dir = fs::temp_directory_path() / ("bench_mem_" + label);
    fs::create_directories(dir);
    return dir;
}

struct MemoryEnv {
    fs::path dir;
    std::unique_ptr<cortrix::CortrixStoreSqlite> store;
    std::unique_ptr<cortrix::MemoryStore> memory_store;

    MemoryEnv(const std::string& label) {
        dir = MakeTempDir(label);
        store = std::make_unique<cortrix::CortrixStoreSqlite>(
            (dir / "bench.db").string());
        store->Open();

        memory_store = std::make_unique<cortrix::MemoryStore>(*store);
        memory_store->Init((dir / "memory.db").string());
    }

    ~MemoryEnv() {
        store->Close();
        fs::remove_all(dir);
    }
};

// ---------------------------------------------------------------------------
// BM_SessionCreate – create memory sessions
// ---------------------------------------------------------------------------

static void BM_SessionCreate(benchmark::State& state) {
    MemoryEnv env("sess_create");

    for (auto _ : state) {
        cortrix::MemorySession session;
        session.namespace_name = "default";
        auto s = env.memory_store->SessionCreate(session);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SessionCreate);

// ---------------------------------------------------------------------------
// BM_InteractionInsert – insert interaction logs
// ---------------------------------------------------------------------------

static void BM_InteractionInsert(benchmark::State& state) {
    MemoryEnv env("int_insert");

    cortrix::MemorySession session;
    session.namespace_name = "default";
    env.memory_store->SessionCreate(session);

    int idx = 0;
    for (auto _ : state) {
        cortrix::InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.user_id = "bench_user";
        log.role = (idx % 2 == 0) ? "user" : "assistant";
        log.content = "Benchmark interaction content number " +
                      std::to_string(idx);
        log.query_type = "semantic";
        log.status = "success";
        env.memory_store->InteractionInsert(log);
        ++idx;
    }
}
BENCHMARK(BM_InteractionInsert);

// ---------------------------------------------------------------------------
// BM_InteractionSearch – search interactions
// ---------------------------------------------------------------------------

static void BM_InteractionSearch(benchmark::State& state) {
    const int scale = static_cast<int>(state.range(0));
    MemoryEnv env("int_search_" + std::to_string(scale));

    cortrix::MemorySession session;
    session.namespace_name = "default";
    env.memory_store->SessionCreate(session);

    // Seed interactions
    for (int i = 0; i < scale; ++i) {
        cortrix::InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.user_id = "bench_user";
        log.role = (i % 2 == 0) ? "user" : "assistant";
        log.content = "Interaction about cortrix semantic search engine " +
                      std::to_string(i);
        log.query_type = "semantic";
        log.status = "success";
        env.memory_store->InteractionInsert(log);
    }

    for (auto _ : state) {
        std::vector<cortrix::InteractionLog> results;
        auto s = env.memory_store->InteractionSearch(
            "default", "semantic search", "", "", 10, results);
        benchmark::DoNotOptimize(results);
    }
}
BENCHMARK(BM_InteractionSearch)->Arg(100)->Arg(1000);

// ---------------------------------------------------------------------------
// BM_InteractionGetRecent – get recent interactions
// ---------------------------------------------------------------------------

static void BM_InteractionGetRecent(benchmark::State& state) {
    MemoryEnv env("int_recent");

    cortrix::MemorySession session;
    session.namespace_name = "default";
    env.memory_store->SessionCreate(session);

    // Seed 100 interactions
    for (int i = 0; i < 100; ++i) {
        cortrix::InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.user_id = "bench_user";
        log.role = (i % 2 == 0) ? "user" : "assistant";
        log.content = "Recent interaction content " + std::to_string(i);
        log.query_type = "semantic";
        log.status = "success";
        env.memory_store->InteractionInsert(log);
    }

    for (auto _ : state) {
        std::vector<cortrix::InteractionLog> interactions;
        auto s = env.memory_store->InteractionGetRecent(
            session.session_id, 10, interactions);
        benchmark::DoNotOptimize(interactions);
    }
}
BENCHMARK(BM_InteractionGetRecent);

// ---------------------------------------------------------------------------
// BM_MemoryInjector – build injection context
// ---------------------------------------------------------------------------

static void BM_MemoryInjector(benchmark::State& state) {
    MemoryEnv env("injector");

    cortrix::MemorySession session;
    session.namespace_name = "default";
    env.memory_store->SessionCreate(session);

    // Seed conversation turns
    for (int i = 0; i < 50; ++i) {
        cortrix::InteractionLog user_log;
        user_log.session_id = session.session_id;
        user_log.namespace_name = "default";
        user_log.user_id = "bench_user";
        user_log.role = "user";
        user_log.content = "What is cortrix semantic search? Question " +
                           std::to_string(i);
        user_log.query_type = "semantic";
        user_log.status = "success";
        env.memory_store->InteractionInsert(user_log);

        cortrix::InteractionLog asst_log;
        asst_log.session_id = session.session_id;
        asst_log.namespace_name = "default";
        asst_log.user_id = "bench_user";
        asst_log.role = "assistant";
        asst_log.content = "Cortrix is a semantic search engine that uses "
                           "vector embeddings and BM25 for hybrid search. "
                           "Response " + std::to_string(i);
        asst_log.query_type = "semantic";
        asst_log.status = "success";
        env.memory_store->InteractionInsert(asst_log);
    }

    cortrix::MemoryConfig mem_cfg;
    mem_cfg.inject_recent_turns = 5;
    mem_cfg.inject_max_tokens = 2000;

    cortrix::MemoryInjector injector(*env.memory_store, mem_cfg);

    for (auto _ : state) {
        auto ctx = injector.GetRecentContext(session.session_id);
        benchmark::DoNotOptimize(ctx);
    }
}
BENCHMARK(BM_MemoryInjector);

// ---------------------------------------------------------------------------
// BM_SessionList – list sessions
// ---------------------------------------------------------------------------

static void BM_SessionList(benchmark::State& state) {
    MemoryEnv env("sess_list");

    // Create 100 sessions
    for (int i = 0; i < 100; ++i) {
        cortrix::MemorySession session;
        session.namespace_name = "default";
        env.memory_store->SessionCreate(session);
    }

    for (auto _ : state) {
        std::vector<cortrix::MemorySession> sessions;
        auto s = env.memory_store->SessionList("default", 20, 0, sessions);
        benchmark::DoNotOptimize(sessions);
    }
}
BENCHMARK(BM_SessionList);
