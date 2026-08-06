#pragma once
#include <optional>
#include <string>

namespace cortrix::server {

/// GC admin CLI (ARCH: gc-status / gc-run / restore / purge). The
/// cortrix-server binary has no separate CLI framework, so these run as
/// subcommands of the server binary against the same catalog.db
/// (`cortrix-server gc-status`, etc.). Agent-first design keeps the SDK ops.gc.*
/// namespace as the primary interface; this CLI is the admin convenience path.
///
/// Returns the process exit code if argv[1] is a recognized gc subcommand
/// (the caller should return it instead of starting the server), or std::nullopt
/// if argv[1] is not a gc subcommand (the caller continues to normal startup).
///
/// Subcommands:
///   gc-status                 -> print the GcStatus snapshot (JSON)
///   gc-run                    -> run one three-stage sweep, print the report
///   purge                     -> immediate purge (bypasses windows), print report
///   restore <doc_id> [...]    -> restore soft-deleted documents, print per-id result
std::optional<int> MaybeRunGcCli(int argc, char* argv[]);

}  // namespace cortrix::server
