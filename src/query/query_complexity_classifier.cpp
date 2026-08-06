#include "cortrix/query/query_complexity_classifier.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "cortrix/query/query_context.h"
#include "cortrix/query/query_router_metrics.h"
#include "cortrix/query/router_error.h"

namespace cortrix::query {

namespace {

// Lower-case + trim an ASCII-ish query for the chat rule guard. Multi-byte UTF-8
// bytes pass through unchanged (the CJK chat phrases below are matched on raw
// bytes). Trailing punctuation (".", "!", "?", "。", "！") is stripped so "Hi!"
// and "hi" match the same pleasantry.
std::string NormalizeForChat(const std::string& query) {
    size_t b = 0;
    size_t e = query.size();
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (b < e && is_space(static_cast<unsigned char>(query[b]))) ++b;
    while (e > b && is_space(static_cast<unsigned char>(query[e - 1]))) --e;

    std::string out;
    out.reserve(e - b);
    for (size_t i = b; i < e; ++i) {
        unsigned char c = static_cast<unsigned char>(query[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else {
            out.push_back(query[i]);
        }
    }
    // Strip trailing ASCII pleasantry punctuation.
    while (!out.empty()) {
        char c = out.back();
        if (c == '.' || c == '!' || c == '?' || c == ',' || c == ' ') {
            out.pop_back();
        } else {
            break;
        }
    }
    return out;
}

// Clamp the effective confidence threshold to the hard limit [0.3, 0.8].
float EffectiveThreshold(const ComplexityConfig& config) {
    float t = config.confidence_threshold;
    if (t < 0.3f) t = 0.3f;
    if (t > 0.8f) t = 0.8f;
    return t;
}

// Map a routing label to the metrics Decision bucket.
QueryRouterMetrics::Decision DecisionForLabel(const std::string& label) {
    if (label == "simple") return QueryRouterMetrics::Decision::kSimple;
    if (label == "chat")   return QueryRouterMetrics::Decision::kChat;
    return QueryRouterMetrics::Decision::kComplex;  // "complex" / unexpected → complex
}

}  // namespace

QueryComplexityClassifier::QueryComplexityClassifier(
    std::shared_ptr<IComplexityClassifierBackend> backend,
    const ComplexityConfig& config)
    : backend_(std::move(backend)), config_(config) {}

bool QueryComplexityClassifier::IsAvailable() const {
    return backend_ != nullptr && backend_->IsAvailable();
}

bool QueryComplexityClassifier::IsChatQuery(const std::string& query) {
    const std::string n = NormalizeForChat(query);
    if (n.empty()) return false;  // empty content is handled as a fail-safe elsewhere
    // Conservative pleasantry set (risk: Chat skips all retrieval, so only a
    // tight match of a content-free greeting/acknowledgment routes here).
    static const char* chat_exact[] = {
        "hi", "hello", "hey", "yo", "hiya",
        "thanks", "thank you", "thx", "ty", "cheers",
        "ok", "okay", "cool", "great", "nice",
        "bye", "goodbye", "good morning", "good afternoon", "good evening",
        "how are you", "what's up", "whats up", "sup",
        // CJK greetings/acknowledgments (raw UTF-8 bytes — functional matching data):
        "\xe4\xbd\xa0\xe5\xa5\xbd",                       // hello
        "\xe5\x97\xa8",                                   // hi
        "\xe8\xb0\xa2\xe8\xb0\xa2",                       // thanks
        "\xe5\xa4\x9a\xe8\xb0\xa2",                       // thanks
        "\xe5\x86\x8d\xe8\xa7\x81",                       // bye
        "\xe6\x97\xa9\xe4\xb8\x8a\xe5\xa5\xbd",           // good morning
    };
    for (const char* g : chat_exact) {
        if (n == g) return true;
    }
    return false;
}

bool QueryComplexityClassifier::HasMultiTurnSignal(const std::string& query) {
    // Lower-case ASCII for whole-word pronoun matching; CJK cues matched on bytes.
    std::string lower;
    lower.reserve(query.size() + 2);
    lower.push_back(' ');  // sentinel so leading-word boundaries are uniform
    for (char c : query) {
        unsigned char uc = static_cast<unsigned char>(c);
        lower.push_back(uc < 0x80 ? static_cast<char>(std::tolower(uc)) : c);
    }
    lower.push_back(' ');

    // English anaphora — matched as " word " to avoid substring false positives
    // (e.g. "it" inside "item"). Follow-up cues kept short + high-signal.
    static const char* en_word_cues[] = {
        " it ", " this ", " that ", " they ", " them ", " these ", " those ",
        " he ", " she ", " its ", " their ",
    };
    for (const char* cue : en_word_cues) {
        if (lower.find(cue) != std::string::npos) return true;
    }
    static const char* en_phrase_cues[] = {
        "what about", "tell me more", "go on", "continue", "and then",
        "the above", "previous", "earlier",
    };
    for (const char* cue : en_phrase_cues) {
        if (lower.find(cue) != std::string::npos) return true;
    }
    // CJK anaphora / follow-up cues (raw UTF-8 bytes — functional matching data):
    static const char* cn_cues[] = {
        "\xe5\xae\x83",                       // it
        "\xe4\xbb\x96",                       // he
        "\xe5\xa5\xb9",                       // she
        "\xe8\xbf\x99\xe4\xb8\xaa",           // this one
        "\xe9\x82\xa3\xe4\xb8\xaa",           // that one
        "\xe7\xbb\xa7\xe7\xbb\xad",           // continue
        "\xe5\x86\x8d\xe8\xaf\xb4\xe8\xaf\xb4",  // say more
        "\xe8\xaf\xa6\xe7\xbb\x86\xe8\xaf\xb4\xe8\xaf\xb4",  // elaborate
        "\xe6\xb7\xb1\xe5\x85\xa5",           // go deeper
    };
    for (const char* cue : cn_cues) {
        if (query.find(cue) != std::string::npos) return true;
    }
    return false;
}

bool QueryComplexityClassifier::ShouldSkipRagFusion(const QueryContext& ctx) {
    // Simple / Chat skip rag-fusion (keep the single original query, no LLM expansion).
    // Empty routing_path (not yet routed) → do NOT skip (fail-safe to Complex).
    if (ctx.chat_path_triggered) return true;
    return ctx.routing_path == "simple" || ctx.routing_path == "chat";
}

bool QueryComplexityClassifier::ShouldSkipCrag(const QueryContext& ctx) {
    // Kept in exact lockstep with retrieval/routing_mock.h::ShouldSkipCrag.
    if (ctx.chat_path_triggered) return true;
    return ctx.routing_path == "simple" || ctx.routing_path == "chat";
}

retrieval::ClassificationResult QueryComplexityClassifier::DegradedResult() {
    retrieval::ClassificationResult r;
    r.label = "complex";       // Fail-safe: default to the full pipeline
    r.score = 0.5f;
    r.confidence = 0.5f;
    r.error_code = RouterErrorCodeString(RouterErrorCode::kInferenceFailed);
    return r;
}

retrieval::ClassificationResult QueryComplexityClassifier::RunClassifier(
    const std::string& query) {
    // L3: retry transient faults with exponential back-off (50/100/200ms),
    // then transparently degrade to Complex. We do NOT actually sleep here — the
    // back-off schedule is a integration concern once a real (network/IPC) backend exists;
    // standalone the loop just bounds the retry count. The boundary stays total.
    const int max_attempts = std::max(1, config_.max_inference_retries);
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        try {
            ComplexityBackendResult b = backend_->Infer(query);
            retrieval::ClassificationResult r;
            r.label = b.label;
            r.score = b.confidence;
            r.confidence = b.confidence;
            return r;  // success (error_code stays unset)
        } catch (const RouterInferenceError&) {
            // transient → retry (or fall through to degrade after the last attempt)
        }
    }
    return DegradedResult();
}

retrieval::ClassificationResult QueryComplexityClassifier::Classify(
    const retrieval::ClassifierInput& input) {
    // This is a pre-retrieval router: only the query text matters; input.chunks is
    // ignored (we honor the shared post-retrieval IClassifier signature anyway).
    // L2: no usable backend → degrade to Complex (classifier_unavailable). We keep
    // the dedicated INFERENCE_FAILED code off this path; the unavailable case is
    // surfaced via RouteAndUpdateContext's routing_decision_source instead.
    if (!IsAvailable()) {
        retrieval::ClassificationResult r;
        r.label = "complex";
        r.score = 0.5f;
        r.confidence = 0.5f;
        return r;
    }

    const auto t0 = std::chrono::steady_clock::now();
    retrieval::ClassificationResult r = RunClassifier(input.query);
    const auto t1 = std::chrono::steady_clock::now();
    QueryRouterMetrics::Instance().ObserveClassifierLatency(
        std::chrono::duration<double>(t1 - t0).count());
    return r;
}

Status QueryComplexityClassifier::RouteAndUpdateContext(
    QueryContext& ctx,
    const std::optional<std::string>& force_route) {
    auto& metrics = QueryRouterMetrics::Instance();

    // Commit a decision onto ctx + record the distribution metric. The metric
    // bucket is taken from the routing label by default; the fail-safe paths pass
    // an explicit `metric` so a degrade-to-Complex counts in the `fallback` bucket
    // (not `complex`) — keeps `fallback` distinct from a confident `complex`.
    auto commit_as = [&](const std::string& path, float score, const char* source,
                         bool chat, QueryRouterMetrics::Decision metric) {
        ctx.routing_path = path;
        ctx.complexity_score = score;
        ctx.routing_decision_source = source;
        ctx.chat_path_triggered = chat;
        metrics.RecordDecision(metric);
    };
    auto commit = [&](const std::string& path, float score, const char* source,
                      bool chat) {
        commit_as(path, score, source, chat, DecisionForLabel(path));
    };

    // --- 1. Agent ?route override (step 1) ---
    if (force_route.has_value()) {
        const std::string& route = force_route.value();
        if (route != "auto" && route != "simple" && route != "complex" &&
            route != "chat") {
            // Invalid token → leave ctx untouched; caller surfaces
            // CX_ERR_ROUTER_FORCE_ROUTE_INVALID (the structured_data carries
            // invalid_route_value at the call site).
            return RouterStatus(RouterErrorCode::kForceRouteInvalid, route);
        }
        if (route != "auto") {
            commit(route, 1.0f, "force_route", route == "chat");
            return Status::Ok();
        }
        // route == "auto" → fall through to NS-config / classifier.
    }

    // --- 2. NS-config force_route override (step 2) ---
    // Standalone: config_ is the resolved ComplexityConfig (defaults / NS-override;
    // the NS-config resolution path is not wired yet). An out-of-set NS force_route is a
    // config error surfaced the same way as the Agent override.
    if (!config_.IsForceRouteValid()) {
        return RouterStatus(RouterErrorCode::kForceRouteInvalid, config_.force_route);
    }
    if (config_.force_route != "auto") {
        commit(config_.force_route, 1.0f, "ns_force_route",
               config_.force_route == "chat");
        return Status::Ok();
    }

    // --- 3. heuristic chat rule guard (step 3) ---
    if (IsChatQuery(ctx.query)) {
        commit("chat", 1.0f, "rule", /*chat=*/true);
        return Status::Ok();
    }

    // --- L1 / L2: NS disabled or backend unavailable → default to Complex ---
    // (L1 / L2). routing_decision_source = "classifier_unavailable". The
    // metric bucket is `fallback` (a fail-safe Complex, not a confident one).
    if (!config_.enabled || !IsAvailable()) {
        commit_as("complex", 0.5f, "classifier_unavailable", /*chat=*/false,
                  QueryRouterMetrics::Decision::kFallback);
        return Status::Ok();
    }

    // --- 4. main path: classifier backend inference (step 4) ---
    const auto t0 = std::chrono::steady_clock::now();
    retrieval::ClassificationResult result = RunClassifier(ctx.query);
    const auto t1 = std::chrono::steady_clock::now();
    metrics.ObserveClassifierLatency(std::chrono::duration<double>(t1 - t0).count());

    // L3 transient inference fault already degraded inside RunClassifier → `fallback`.
    if (result.error_code.has_value()) {
        commit_as("complex", result.confidence, "inference_failed_fallback",
                  /*chat=*/false, QueryRouterMetrics::Decision::kFallback);
        return Status::Ok();
    }

    // --- 5. confidence-threshold fail-safe (step 5) → `fallback` ---
    const float threshold = EffectiveThreshold(config_);
    if (result.confidence < threshold) {
        commit_as("complex", result.confidence, "low_confidence_fallback",
                  /*chat=*/false, QueryRouterMetrics::Decision::kFallback);
        return Status::Ok();
    }

    // --- 6. write the classifier decision (step 6) ---
    // Chat demotion guard (integration r2 S4 finding): the Adaptive-RAG training labels
    // map "chat" to SQuAD2-unanswerable patterns, so natural full-sentence QA
    // ("Which planet is the hottest...?") can classify as chat - and the chat
    // path SKIPS retrieval entirely, turning a misroute into empty results.
    // Chat is therefore double-gated: the ML label alone never selects it; only
    // the rule guard (step 3, already passed by now = not chat) can. An ML
    // "chat" verdict demotes to the Complex fail-safe and is observable via
    // routing_decision_source. simple/complex misroutes stay cheap (they only
    // change which enrichment stages run), so they keep the direct ML commit.
    if (result.label == "chat") {
        commit_as("complex", result.confidence, "chat_demoted_guard",
                  /*chat=*/false, QueryRouterMetrics::Decision::kFallback);
        return Status::Ok();
    }
    // DistilBERT-tiny is an ML model → routing_decision_source = "llm".
    commit(result.label, result.confidence, "llm", result.label == "chat");

    // --- 7. multi-turn signal detection (borrowed 6) ---
    if (config_.multi_turn_warning_enabled && HasMultiTurnSignal(ctx.query)) {
        ctx.multi_turn_context_warning = true;
    }

    return Status::Ok();
}

}  // namespace cortrix::query
