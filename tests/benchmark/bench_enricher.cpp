#include <benchmark/benchmark.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/enricher_config_resolver.h"
#include "cortrix/spc_enricher/enricher_response_parser.h"
#include "cortrix/spc_enricher/prompt_template.h"

// Enricher single-component Google Benchmark (Issue 6.5 — GB single-component layer). Pure-CPU, no network:
// batch prompt assembly + JSON response parse + NS config JSONB parse. The
// end-to-end pgbench SLA (remote OpenAI / local vLLM) is/integration (needs a live
// endpoint) — see design

namespace {

std::vector<cortrix::spc::ChunkContext> MakeChunks(int n) {
    std::vector<cortrix::spc::ChunkContext> v;
    for (int i = 0; i < n; ++i) {
        cortrix::spc::ChunkContext c;
        c.chunk_text =
            "This is the text of chunk number " + std::to_string(i) +
            ", containing several sentences so the prompt assembly does realistic "
            "string work across a batch of chunks for benchmarking purposes.";
        c.chunk_index = i;
        v.push_back(std::move(c));
    }
    return v;
}

std::string MakeResponseBody(int n) {
    nlohmann::json obj = nlohmann::json::object();
    for (int i = 0; i < n; ++i) {
        obj[std::to_string(i)] = {
            {"entities", nlohmann::json::array(
                             {{{"text", "Entity" + std::to_string(i)},
                               {"type", "ORG"},
                               {"start_offset", 0},
                               {"end_offset", 6}},
                              {{"text", "Person" + std::to_string(i)},
                               {"type", "PERSON"},
                               {"start_offset", 7},
                               {"end_offset", 13}}})},
            {"summary", "summary of chunk " + std::to_string(i)},
            {"score", 0.7},
        };
    }
    return obj.dump();
}

}  // namespace

// --- batch prompt assembly (S2.3) -------------------------------------------
static void BM_PromptRender(benchmark::State& state) {
    const int batch = static_cast<int>(state.range(0));
    cortrix::spc::PromptTemplate tpl("default-zh");
    auto chunks = MakeChunks(batch);
    for (auto _ : state) {
        std::string prompt = tpl.Render(chunks);
        benchmark::DoNotOptimize(prompt);
    }
    state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_PromptRender)->Arg(1)->Arg(8)->Arg(32);

// --- response JSON parse + L1/L2/L3 fault tolerance (S2.4) -------------------
static void BM_ResponseParse(benchmark::State& state) {
    const int batch = static_cast<int>(state.range(0));
    const std::string body = MakeResponseBody(batch);
    for (auto _ : state) {
        auto results = cortrix::spc::ParseEnrichBatchResponse(body, batch, "gpt-4o-mini",
                                                              "default-zh");
        benchmark::DoNotOptimize(results.data());
    }
    state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_ResponseParse)->Arg(1)->Arg(8)->Arg(32);

// --- NS enricher_config JSONB parse (S5.2) ----------------------------------
static void BM_NsConfigParse(benchmark::State& state) {
    const std::string blob =
        R"({"enabled":true,"model":"gpt-4o","score_threshold":0.5,"prompt_template_id":"default-en"})";
    for (auto _ : state) {
        auto r = cortrix::spc::EnricherNsConfig::Parse(blob);
        benchmark::DoNotOptimize(r.ok());
    }
}
BENCHMARK(BM_NsConfigParse);
