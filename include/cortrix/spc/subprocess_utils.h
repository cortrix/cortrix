#pragma once
#include <string>
#include <vector>
#include "cortrix/common/status.h"

namespace cortrix {

/// Run a subprocess with argv array (no shell) and capture stdout.
/// This is the preferred API — it avoids shell injection by using
/// fork+execvp directly instead of popen/system.
/// @param args: argument vector where args[0] is the executable
/// @param timeout_s: timeout in seconds (<=0 means no timeout).
///        If the child process does not exit within timeout_s,
///        it is killed with SIGKILL and an Internal error is returned.
/// @param output: [out] captured stdout
/// @return Ok / Internal(process failed or timed out)
Status RunSubprocess(const std::vector<std::string>& args, int timeout_s, std::string* output);

/// Check if an executable exists in PATH
/// @param executable: executable name (e.g., "python3", "git")
/// @return true if executable is found, false otherwise
bool CheckExecutable(const std::string& executable);

}  // namespace cortrix
