/// @file fuzz_flatten_metadata.cpp
/// @brief libFuzzer harness for FlattenMetadataIntoMap (metadata_json parse+flatten).
///
/// Target: cortrix::FlattenMetadataIntoMap (src/query/live_single_unit_executor.cpp).
/// It takes a raw metadata_json string straight off a stored block row, runs
/// json::parse on it, and flattens the top-level object into a string->string map
/// (string values verbatim, everything else via .dump()). This is the FiQA result
/// identity path (R9 Bug 1, commit b7323478). The parse + the .dump() of arbitrary
/// nested values are the surfaces of interest (deep nesting, huge numbers, etc.).
///
/// WHY A LOCAL COPY: the function is defined in live_single_unit_executor.cpp, whose
/// TU pulls the entire live executor (pool/embedder/fusion/reranker). The body below
/// is copied VERBATIM (commit release/v1.0.0-rc.1). DRIFT GUARD: keep in sync with
/// the real FlattenMetadataIntoMap. Build via build_fuzzers.sh.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// VERBATIM copy of cortrix::FlattenMetadataIntoMap (see file header drift guard).
void FlattenMetadataIntoMap(const std::string& metadata_json,
                            std::map<std::string, std::string>& out) {
    if (metadata_json.empty()) return;
    try {
        json parsed = json::parse(metadata_json);
        if (!parsed.is_object()) return;  // only object-shaped metadata flattens
        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
            out[it.key()] = it.value().is_string()
                                ? it.value().get<std::string>()
                                : it.value().dump();
        }
    } catch (...) {
        // Invalid metadata JSON: ignore, leave the map as-is.
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string metadata(reinterpret_cast<const char*>(data), size);
    std::map<std::string, std::string> out;
    FlattenMetadataIntoMap(metadata, out);
    (void)out;
    return 0;
}
