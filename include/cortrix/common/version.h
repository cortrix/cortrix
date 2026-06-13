#pragma once

namespace cortrix {

/// The single source of truth for the Cortrix server version string (issue ⑭ —
/// D3.5 r2 · Wave P · P5). Surfaced by GET /api/v1/system/version, the
/// /system/health/{live,ready} probes, the /health endpoint, and the
/// cortrix_build_info metric. Keep this aligned with the CMake project() VERSION
/// and the deploy/Dockerfile LABEL.
constexpr const char* kCortrixVersion = "1.0.0";

}  // namespace cortrix
