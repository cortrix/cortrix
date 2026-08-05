#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/spc/parser_subprocess.h"

// Parser branch coverage for parser_subprocess.cpp (RunParserSubprocess fork/exec/
// poll primitive). The success / launch-failure / timeout arms are covered in
// test_docling_parser.cpp; this file targets the remaining branches:
//   - empty argv (launched=false short-circuit)
//   - stdout overflow → output_truncated + child killed (DrainFd cap branch)
//   - stderr capture (the err_idx drain branch)
//   - no-timeout path (has_timeout false: poll_ms = -1)
//   - a signal-killed child (WIFEXITED false → exit_code = -1)
//   - exit-within-grace vs SIGKILL after a timeout
// All black-box via /bin/sh scripts; no product code is touched.
namespace cortrix::spc {
namespace {

constexpr int64_t kBig = 1 << 20;  // 1 MiB stdout cap (generous)

// Empty args → the function returns immediately with launched=false (no fork).
TEST(ParserSubprocessBranchesTest, EmptyArgsNotLaunched) {
    SubprocessResult r = RunParserSubprocess({}, 5000, kBig);
    EXPECT_FALSE(r.launched);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, -1);
}

// stdout exceeds the cap → DrainFd hits the overflow branch, sets
// output_truncated, and the parent kills the child. The captured stdout is
// truncated to (at most) the cap.
TEST(ParserSubprocessBranchesTest, StdoutOverflowTruncatesAndKills) {
    // Emit ~64KB but cap at 1KB → overflow.
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "yes ABCDEFGH | head -c 65536"}, 5000,
        /*max_output_bytes=*/1024, /*kill_grace_ms=*/500);
    EXPECT_TRUE(r.launched);
    EXPECT_TRUE(r.output_truncated);
    EXPECT_LE(static_cast<int64_t>(r.stdout_data.size()), 1024);
}

// A no-timeout call (timeout_ms <= 0) takes the has_timeout=false poll path
// (poll_ms = -1, blocking) and still completes + reports the exit code.
TEST(ParserSubprocessBranchesTest, NoTimeoutBlockingPollCompletes) {
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "printf done; exit 0"}, /*timeout_ms=*/0, kBig);
    EXPECT_TRUE(r.launched);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_data, "done");
}

// stderr is captured independently of stdout (the err_idx drain branch). The
// child writes to both fds; both must land in their respective buffers.
TEST(ParserSubprocessBranchesTest, StderrCapturedSeparately) {
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "printf out; printf err 1>&2; exit 0"}, 5000, kBig);
    EXPECT_TRUE(r.launched);
    EXPECT_EQ(r.stdout_data, "out");
    EXPECT_NE(r.stderr_data.find("err"), std::string::npos);
}

// A non-zero exit is reported faithfully (the WIFEXITED true branch with a
// non-zero WEXITSTATUS).
TEST(ParserSubprocessBranchesTest, NonZeroExitReported) {
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "exit 7"}, 5000, kBig);
    EXPECT_TRUE(r.launched);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 7);
}

// A child killed by a signal → WIFEXITED is false → exit_code = -1 (the ternary's
// false arm at the final waitpid).
TEST(ParserSubprocessBranchesTest, SignalKilledChildExitMinusOne) {
    // The shell sends SIGKILL to itself; the process is signaled, not exited.
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "kill -KILL $$"}, 5000, kBig);
    EXPECT_TRUE(r.launched);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, -1);
}

// Timeout where the child exits promptly on SIGTERM → the grace-window waitpid
// reaps it before SIGKILL (the "exited within grace" return branch). The child
// installs no SIGTERM handler, so the default action terminates it quickly.
TEST(ParserSubprocessBranchesTest, TimeoutChildExitsWithinGrace) {
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "sleep 10"}, /*timeout_ms=*/150, kBig,
        /*kill_grace_ms=*/2000);
    EXPECT_TRUE(r.launched);
    EXPECT_TRUE(r.timed_out);
}

// Timeout where the child IGNORES SIGTERM → the grace window elapses and the
// parent escalates to SIGKILL (the post-grace kill branch). 'trap "" TERM' makes
// SIGTERM a no-op so only SIGKILL can end it.
TEST(ParserSubprocessBranchesTest, TimeoutChildIgnoresTermEscalatesToKill) {
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "trap '' TERM; sleep 10"}, /*timeout_ms=*/150, kBig,
        /*kill_grace_ms=*/200);
    EXPECT_TRUE(r.launched);
    EXPECT_TRUE(r.timed_out);
}

// max_output_bytes <= 0 → the cap is disabled (the cap>0 guard false branch); a
// modest output is captured in full.
TEST(ParserSubprocessBranchesTest, UnlimitedOutputCapDisabled) {
    SubprocessResult r = RunParserSubprocess(
        {"/bin/sh", "-c", "printf '0123456789'"}, 5000, /*max_output_bytes=*/0);
    EXPECT_TRUE(r.launched);
    EXPECT_EQ(r.stdout_data, "0123456789");
}

}  // namespace
}  // namespace cortrix::spc
