#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cortrix::spc {

/// Structured outcome of a parser bridge subprocess. Richer than the
/// shared RunSubprocess (which collapses every failure to one Status): the
/// parser needs to tell PARSE_TIMEOUT / SUBPROCESS_CRASHED / SUBPROCESS_FAILED /
/// OUTPUT_TOO_LARGE apart and surface exit_code + stderr tail into the
/// Agent-friendly structured_data. Kept local to the spc module so the shared
/// subprocess util stays untouched during parallel Wave-A work.
struct SubprocessResult {
    bool launched = false;       ///< false = exec failed (e.g. python missing)
    bool timed_out = false;      ///< killed after exceeding timeout
    bool output_truncated = false; ///< stdout exceeded max_output_bytes → killed
    int exit_code = -1;          ///< child exit code (valid when launched && !timed_out)
    std::string stdout_data;     ///< captured stdout (the JSON payload)
    std::string stderr_data;     ///< captured stderr (logs / python traceback)
};

/// Run `args` (args[0] = executable, no shell) capturing stdout + stderr with a
/// wall-clock timeout and a stdout size cap. Mirrors the fork/exec/poll +
/// SIGTERM→SIGKILL pattern of the MVP RunSubprocess, extended to report the
/// structured outcome above.
///
/// @param args            argv; args[0] is the executable (looked up via PATH)
/// @param timeout_ms      wall-clock budget (<=0 = no timeout). On expiry the
///                        child gets SIGTERM, then SIGKILL after `kill_grace_ms`.
/// @param max_output_bytes stdout cap (<=0 = unlimited). On overflow the child is
///                        killed and output_truncated=true.
/// @param kill_grace_ms   SIGTERM→SIGKILL grace window (default 5s per).
/// @return SubprocessResult (never throws; launch failure → launched=false).
SubprocessResult RunParserSubprocess(const std::vector<std::string>& args,
                                     int timeout_ms,
                                     int64_t max_output_bytes,
                                     int kill_grace_ms = 5000);

}  // namespace cortrix::spc
