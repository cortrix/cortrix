#pragma once
#include <ostream>
#include <string>

#include "cortrix/onnx/startup_validator.h"

namespace cortrix {
struct CortrixConfig;
}

namespace cortrix::onnx {

/// `cortrix-server --check-onnx` dry-run logic (F22 §9.4). Renders the human-
/// readable [OK]/[FAIL] report from a StartupValidator::ValidationReport and
/// returns the process exit code (0 = all checks passed, 1 = a check failed).
/// On failure the report goes to `err` (stderr) with the CX_ERR_* code +
/// structured fields; on success the per-check OK lines go to `out` (stdout).
///
/// Pure rendering over a given report → fully testable with no real server /
/// no real models. This is the dry-run path's core; the `main.cpp` argv
/// dispatch that builds the report from a loaded config and calls this is the
/// thin D3.5 wiring (see RunCheckOnnxCli).
int RenderCheckOnnxReport(const StartupValidator::ValidationReport& report,
                          std::ostream& out, std::ostream& err);

/// Full `--check-onnx` entry: collect registered models from `config`, run
/// StartupValidator::ValidateVerbose, render via RenderCheckOnnxReport. Returns
/// the exit code. Standalone-usable, but its CALLER (the argv branch in
/// main.cpp, before server boot) is D3.5 wiring — kept out of this Story's
/// scope to honor the D3 standalone rule.
int RunCheckOnnxCli(const CortrixConfig& config,
                    std::ostream& out, std::ostream& err);

}  // namespace cortrix::onnx
