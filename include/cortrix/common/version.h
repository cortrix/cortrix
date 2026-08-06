#pragma once

namespace cortrix {

/// The single source of truth for the Cortrix server version string (issue ⑭ —
/// integration r2 · Wave P · P5). Surfaced by GET /api/v1/system/version, the
/// /system/health/{live,ready} probes, the /health endpoint, and the
/// cortrix_build_info metric. This value is derived from the root VERSION file
/// by scripts/sync_version.py; do not edit it independently.
constexpr const char* kCortrixVersion = "1.0.0-rc.1";

}  // namespace cortrix
