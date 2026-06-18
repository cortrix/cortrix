/// @file fuzz_query_request_fromjson.cpp
/// @brief libFuzzer harness for the single-NS /api/v1/query body parse path.
///
/// Mirrors src/query/query_routes.cpp Step-1 exactly: the raw request body is fed
/// to json::parse inside a try/catch(...) (malformed JSON => InvalidArgument), then
/// QueryRequest::FromJson(body, &req) -> Normalize() -> Validate(). The fuzzer drives
/// the WHOLE chain because the json::parse(req.body) recursion (deep nesting) lives
/// at the route layer, not inside FromJson -- that is the most interesting parse
/// surface for a stack-exhaustion / UB hunt.
///
/// Compiles the real src/query/query_request.cpp. Build via build_fuzzers.sh.

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/query/query_request.h"

using json = nlohmann::json;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string body_str(reinterpret_cast<const char*>(data), size);

    // Step 1: parse -- guarded identically to query_routes.cpp:80-85.
    json body;
    try {
        body = json::parse(body_str);
    } catch (...) {
        return 0;  // malformed JSON is a handled 400; not a bug.
    }

    // Step 2: structured field extraction (the real parse entry under test).
    cortrix::QueryRequest req;
    cortrix::Status st = cortrix::QueryRequest::FromJson(body, &req);
    if (!st.ok()) return 0;

    // Step 3: post-parse transforms the handler always runs on accepted input.
    req.Normalize();
    cortrix::Status vst = req.Validate();
    (void)vst;
    return 0;
}
