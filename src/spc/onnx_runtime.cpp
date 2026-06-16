#include <cstdint>
#include "cortrix/onnx/runtime.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#ifdef CORTRIX_HAS_ONNXRUNTIME
#include <onnxruntime_c_api.h>
#endif

namespace cortrix::onnx {

namespace {

// Stringify the CMake-injected CORTRIX_ONNXRT_EXPECTED_MAJOR token. The flag is
// a bare integer (e.g. -DCORTRIX_ONNXRT_EXPECTED_MAJOR=1), so we stringize it.
#define CORTRIX_STR2(x) #x
#define CORTRIX_STR(x) CORTRIX_STR2(x)

// Parse the leading integer of a dotted version string ("1.17.1" -> 1).
// Returns 0 if the string does not start with a digit.
int ParseLeadingInt(const std::string& v) {
    std::size_t i = 0;
    int value = 0;
    bool any = false;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
        value = value * 10 + (v[i] - '0');
        any = true;
        ++i;
    }
    return any ? value : 0;
}

// Parse the second dotted component ("1.17.1" -> 17). Returns 0 if absent.
int ParseMinor(const std::string& v) {
    std::size_t dot = v.find('.');
    if (dot == std::string::npos) return 0;
    return ParseLeadingInt(v.substr(dot + 1));
}

// ONNX Runtime does not expose the supported opset range programmatically, so we
// derive it from the loaded runtime's major/minor against the public ORT release
// "opset support" matrix. For the 1.x line the lower bound has long been opset 7
// and the upper bound advances roughly one opset per minor release. This is a
// thin lookup (D4=A) — when a future minor is loaded that is newer than the
// table, we extrapolate the upper bound from the last known anchor rather than
// failing, and clamp anything older to the floor. Cross-major (2.x) is out of
// the F22 V1.0 scope and returns the conservative {0, 0} "unknown".
constexpr int32_t kOpsetFloorMajor1 = 7;

// Anchors: ORT 1.<minor> max supported opset. From ORT release notes.
//   1.17 -> 19, 1.18 -> 20, 1.19 -> 21, 1.20 -> 22, 1.21 -> 22
// (slope ~ +1 opset / minor up to 1.19, then flattening). Below 1.17 we floor
// the max at 19's predecessor scale; the exact historical values are not needed
// for V1.0 since the shipped runtime is 1.17.1.
std::pair<int32_t, int32_t> DeriveOpsetRangeForMajor1(int minor) {
    int32_t max_opset;
    if (minor <= 17) {
        max_opset = 19;
    } else if (minor <= 21) {
        max_opset = static_cast<int32_t>(19 + (minor - 17));  // 18->20,19->21,20->22,21->23
        max_opset = std::min<int32_t>(max_opset, 22);          // flatten at 22 (1.20/1.21)
    } else {
        // Newer than our table: extrapolate conservatively at the last anchor.
        max_opset = 22;
    }
    return {kOpsetFloorMajor1, max_opset};
}

}  // namespace

bool Runtime::IsRuntimeLinked() {
#ifdef CORTRIX_HAS_ONNXRUNTIME
    return true;
#else
    return false;
#endif
}

std::string Runtime::GetRuntimeVersion() {
#ifdef CORTRIX_HAS_ONNXRUNTIME
    // OrtGetApiBase() is the stable entry point present across all 1.x ABI
    // versions; GetVersionString() returns the loaded runtime's version, e.g.
    // "1.17.1". This is what actually changes when a user swaps the `.so`.
    const char* v = OrtGetApiBase()->GetVersionString();
    return v != nullptr ? std::string(v) : std::string();
#else
    return std::string();
#endif
}

int Runtime::GetRuntimeMajorVersion() {
    const std::string v = GetRuntimeVersion();
    if (v.empty()) return 0;
    return ParseLeadingInt(v);
}

std::pair<int32_t, int32_t> Runtime::GetSupportedOpsetRange() {
    const std::string v = GetRuntimeVersion();
    if (v.empty()) return {0, 0};
    const int major = ParseLeadingInt(v);
    if (major == 1) {
        return DeriveOpsetRangeForMajor1(ParseMinor(v));
    }
    // Other majors (notably the future 2.x) are out of F22 V1.0 scope.
    return {0, 0};
}

std::string Runtime::GetCompiledMajorVersion() {
#ifdef CORTRIX_ONNXRT_EXPECTED_MAJOR
    return std::string(CORTRIX_STR(CORTRIX_ONNXRT_EXPECTED_MAJOR));
#else
    // Should always be defined by Dependencies.cmake; defensive default keeps
    // the getter total if someone builds the file in isolation.
    return std::string("1");
#endif
}

#undef CORTRIX_STR
#undef CORTRIX_STR2

}  // namespace cortrix::onnx
