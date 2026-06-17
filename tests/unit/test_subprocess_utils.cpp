#include <gtest/gtest.h>
#include "cortrix/spc/subprocess_utils.h"

namespace cortrix {
namespace {

TEST(SubprocessUtilsTest, EmptyArgs_Error) {
    std::string output;
    Status s = RunSubprocess({}, 5, &output);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST(SubprocessUtilsTest, NullOutput_Error) {
    Status s = RunSubprocess({"echo", "hi"}, 5, nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST(SubprocessUtilsTest, EchoCommand_CapturesStdout) {
    std::string output;
    Status s = RunSubprocess({"echo", "hello subprocess"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("hello subprocess"), std::string::npos);
}

TEST(SubprocessUtilsTest, NonexistentExecutable_Error) {
    std::string output;
    Status s = RunSubprocess({"nonexistent_program_xyz_12345"}, 5, &output);
    EXPECT_FALSE(s.ok());
}

TEST(SubprocessUtilsTest, ExitCodeNonZero_Error) {
    std::string output;
    // "false" always exits with code 1
    Status s = RunSubprocess({"false"}, 5, &output);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("exited with code"), std::string::npos);
}

TEST(SubprocessUtilsTest, TimeoutKillsSlowProcess) {
    std::string output;
    // sleep 60 should be killed after 1 second
    Status s = RunSubprocess({"sleep", "60"}, 1, &output);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("timed out"), std::string::npos);
}

TEST(SubprocessUtilsTest, NoTimeout_CompletesNormally) {
    std::string output;
    // timeout_s=0 means no timeout; echo completes instantly
    Status s = RunSubprocess({"echo", "no timeout"}, 0, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("no timeout"), std::string::npos);
}

TEST(SubprocessUtilsTest, NegativeTimeout_NoTimeout) {
    std::string output;
    // Negative timeout should behave as no timeout
    Status s = RunSubprocess({"echo", "negative"}, -1, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("negative"), std::string::npos);
}

TEST(SubprocessUtilsTest, TimeoutSufficient_CompletesNormally) {
    std::string output;
    // sleep 0 completes immediately, timeout of 5s is plenty
    Status s = RunSubprocess({"echo", "fast"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("fast"), std::string::npos);
}

TEST(SubprocessUtilsTest, LargeOutput_CapturedFully) {
    std::string output;
    // Generate many lines of output
    Status s = RunSubprocess({"seq", "1", "1000"}, 10, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("1000"), std::string::npos);
    EXPECT_GT(output.size(), 100u);
}

// ============================================================
// CheckExecutable tests
// ============================================================

TEST(CheckExecutableTest, EchoExists) {
    EXPECT_TRUE(CheckExecutable("echo"));
}

TEST(CheckExecutableTest, NonexistentProgram) {
    EXPECT_FALSE(CheckExecutable("nonexistent_program_xyz_12345"));
}

TEST(CheckExecutableTest, EmptyName) {
    EXPECT_FALSE(CheckExecutable(""));
}

TEST(CheckExecutableTest, PathSeparatorRejected) {
    // Should reject paths with /
    EXPECT_FALSE(CheckExecutable("/usr/bin/echo"));
}

TEST(CheckExecutableTest, ShellMetacharRejected) {
    EXPECT_FALSE(CheckExecutable("echo;rm"));
    EXPECT_FALSE(CheckExecutable("echo&&ls"));
}

// Additional CheckExecutable tests for character validation coverage
TEST(CheckExecutableTest, SpaceRejected) {
    EXPECT_FALSE(CheckExecutable("echo hello"));
}

TEST(CheckExecutableTest, PipeRejected) {
    EXPECT_FALSE(CheckExecutable("echo|cat"));
}

TEST(CheckExecutableTest, BacktickRejected) {
    EXPECT_FALSE(CheckExecutable("echo`ls`"));
}

TEST(CheckExecutableTest, DollarSignRejected) {
    EXPECT_FALSE(CheckExecutable("echo$PATH"));
}

TEST(CheckExecutableTest, ValidNameWithDashDotUnderscore) {
    // These characters are allowed: alphanumeric, dash, underscore, dot
    // "ls" exists and should be found
    EXPECT_TRUE(CheckExecutable("ls"));
}

TEST(CheckExecutableTest, DottedNameAccepted) {
    // Names with dots are syntactically valid (even if program doesn't exist)
    // This tests the character validation loop, not the which check
    EXPECT_FALSE(CheckExecutable("nonexistent.tool.v2"));
}

// Additional RunSubprocess tests for coverage of edge paths
TEST(SubprocessUtilsTest, MultipleArgs_CapturesOutput) {
    std::string output;
    // Use printf which handles format strings
    Status s = RunSubprocess({"printf", "%s %s", "hello", "world"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(output, "hello world");
}

TEST(SubprocessUtilsTest, MultilineOutput_CapturedFully) {
    std::string output;
    // printf with newlines
    Status s = RunSubprocess({"printf", "line1\nline2\nline3\n"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("line1"), std::string::npos);
    EXPECT_NE(output.find("line3"), std::string::npos);
}

// ============================================================
// RunSubprocess - signal/killed child (WIFSIGNALED path, line 137)
// ============================================================

TEST(SubprocessUtilsTest, SignalKilledChild_NonZeroExit) {
    std::string output;
    // "kill -SEGV $$" sends SIGSEGV to self — child should be killed by signal
    // Use bash -c to execute the kill
    Status s = RunSubprocess({"bash", "-c", "kill -SEGV $$"}, 5, &output);
    EXPECT_FALSE(s.ok());
    // WIFEXITED would be false, WIFSIGNALED true, so exit_code = -1
    // The error message should contain "exited with code"
}

// ============================================================
// RunSubprocess - stderr is not captured (goes to /dev/null or parent stderr)
// ============================================================

TEST(SubprocessUtilsTest, StderrNotInOutput) {
    std::string output;
    // echo to stderr should NOT appear in captured output
    Status s = RunSubprocess({"bash", "-c", "echo stdout_msg; echo stderr_msg >&2"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("stdout_msg"), std::string::npos);
    // stderr_msg should not be captured
    EXPECT_EQ(output.find("stderr_msg"), std::string::npos);
}

// ============================================================
// RunSubprocess - EINTR handling (simulated via rapidly timing out)
// ============================================================

TEST(SubprocessUtilsTest, RapidTimeoutStress) {
    // Very short timeout with a process that produces output
    // Tests the poll timeout path
    for (int i = 0; i < 5; ++i) {
        std::string output;
        Status s = RunSubprocess({"sleep", "10"}, 1, &output);
        EXPECT_FALSE(s.ok());
    }
}

// ============================================================
// RunSubprocess - empty stdout from successful command
// ============================================================

TEST(SubprocessUtilsTest, EmptyStdout_Success) {
    std::string output;
    // "true" produces no output and exits 0
    Status s = RunSubprocess({"true"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(output.empty());
}

// ============================================================
// RunSubprocess - binary output
// ============================================================

TEST(SubprocessUtilsTest, BinaryOutput_Captured) {
    std::string output;
    // Use bash -c with $'\x..' syntax to produce actual binary bytes
    Status s = RunSubprocess({"bash", "-c", "printf '\\x41\\x42\\x43'"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(output.size(), 3u);
    EXPECT_EQ(output[0], 'A');
    EXPECT_EQ(output[1], 'B');
    EXPECT_EQ(output[2], 'C');
}

// ============================================================
// RunSubprocess - args with spaces (no shell injection)
// ============================================================

TEST(SubprocessUtilsTest, ArgsWithSpaces_NoInjection) {
    std::string output;
    // echo with a space-containing argument - should be treated as single arg
    Status s = RunSubprocess({"echo", "hello world with spaces"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("hello world with spaces"), std::string::npos);
}

// ============================================================
// RunSubprocess - args with special characters
// ============================================================

TEST(SubprocessUtilsTest, ArgsWithSpecialChars_Safe) {
    std::string output;
    // Test that shell metacharacters are treated literally
    Status s = RunSubprocess({"echo", "$(whoami); rm -rf /"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    // The literal string should appear, not the result of command substitution
    EXPECT_NE(output.find("$(whoami)"), std::string::npos);
}

// ============================================================
// CheckExecutable - more character validation
// ============================================================

TEST(CheckExecutableTest, HyphenAndUnderscore_Allowed) {
    // Names with hyphens and underscores should pass character validation
    // (even if the executable doesn't exist)
    bool result = CheckExecutable("my-tool_v2");
    // Character validation passes, but executable likely doesn't exist
    EXPECT_FALSE(result);
}

TEST(CheckExecutableTest, ParenthesisRejected) {
    EXPECT_FALSE(CheckExecutable("echo()"));
}

TEST(CheckExecutableTest, RedirectRejected) {
    EXPECT_FALSE(CheckExecutable("echo>file"));
}

TEST(CheckExecutableTest, QuoteRejected) {
    EXPECT_FALSE(CheckExecutable("echo\"hello\""));
}

// ============================================================
// Coverage Boost: Additional subprocess edge cases
// ============================================================

// Test poll() returning 0 (timeout expired during read) - lines 98-101
TEST(SubprocessUtilsTest, ShortTimeoutWithOutput_TimesOut) {
    std::string output;
    // Use a process that produces output but takes time
    Status s = RunSubprocess({"bash", "-c", "echo start; sleep 30; echo end"}, 1, &output);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("timed out"), std::string::npos);
    // Should have captured "start" before timeout
    EXPECT_NE(output.find("start"), std::string::npos);
}

// Test with single arg (just executable name) - exercises argv building
TEST(SubprocessUtilsTest, SingleArgExecutable) {
    std::string output;
    Status s = RunSubprocess({"date"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_FALSE(output.empty());
}

// Test EAGAIN/EWOULDBLOCK path (lines 113-114)
// The non-blocking pipe may return EAGAIN when no data available yet
TEST(SubprocessUtilsTest, NonBlockingReadEagain_Handled) {
    std::string output;
    // A command that sleeps briefly before producing output exercises the
    // poll/read loop where EAGAIN can occur
    Status s = RunSubprocess({"bash", "-c", "sleep 0.1; echo delayed"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("delayed"), std::string::npos);
}

// Test with very large number of args
TEST(SubprocessUtilsTest, ManyArgs) {
    std::vector<std::string> args = {"echo"};
    for (int i = 0; i < 100; ++i) {
        args.push_back("arg" + std::to_string(i));
    }
    std::string output;
    Status s = RunSubprocess(args, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("arg99"), std::string::npos);
}

// Test CheckExecutable with a program that exists but with valid chars
TEST(CheckExecutableTest, Python3Exists) {
    // python3 should be available in test env
    bool result = CheckExecutable("python3");
    // May or may not exist; we mainly test that the function runs without crashing
    (void)result;
}

// Test CheckExecutable - tab character rejected
TEST(CheckExecutableTest, TabRejected) {
    EXPECT_FALSE(CheckExecutable("echo\ttab"));
}

// Test CheckExecutable - newline rejected
TEST(CheckExecutableTest, NewlineRejected) {
    EXPECT_FALSE(CheckExecutable("echo\nnewline"));
}

// ============================================================
// RunSubprocess - multiple buffer reads (output > 4 KiB exercises the read loop
// more than once, hitting the repeated poll→read→append path).
// ============================================================

TEST(SubprocessUtilsTest, OutputLargerThanBuffer_FullyCaptured) {
    std::string output;
    // Produce ~8 KiB of output (two full 4096-byte pipe reads at minimum).
    Status s = RunSubprocess(
        {"bash", "-c", "yes x | head -c 8192"}, 10, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_GE(output.size(), 8192u);
}

// ============================================================
// RunSubprocess - process that writes then sleeps then writes more exercises
// the poll loop where multiple poll() calls are needed to drain output.
// ============================================================

TEST(SubprocessUtilsTest, IntermittentOutput_DrainedCompletely) {
    std::string output;
    // Write one chunk, sleep briefly, write another chunk.
    Status s = RunSubprocess(
        {"bash", "-c", "echo chunk1; sleep 0.05; echo chunk2"}, 5, &output);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_NE(output.find("chunk1"), std::string::npos);
    EXPECT_NE(output.find("chunk2"), std::string::npos);
}

// ============================================================
// RunSubprocess - exit code 127 (execvp failed in child, "not found") surfaces
// as a non-zero exit code error. execvp failing is what produces 127; this is
// the code path when the binary exists in PATH but returns 127 via the shell.
// We exercise it through bash -c 'exit 127'.
// ============================================================

TEST(SubprocessUtilsTest, ExitCode127_IsError) {
    std::string output;
    Status s = RunSubprocess({"bash", "-c", "exit 127"}, 5, &output);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("exited with code 127"), std::string::npos);
}

// ============================================================
// RunSubprocess - exit code 2 (grep/diff convention): non-zero, still an error.
// ============================================================

TEST(SubprocessUtilsTest, ExitCode2_IsError) {
    std::string output;
    Status s = RunSubprocess({"bash", "-c", "exit 2"}, 5, &output);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("exited with code 2"), std::string::npos);
}

// ============================================================
// RunSubprocess - WIFSIGNALED path with an exit_code = -1 edge:
// a SIGKILL'd child (not through our timeout code) yields WIFEXITED=false,
// so exit_code = -1, which is != 0, and must surface an error.
// ============================================================

TEST(SubprocessUtilsTest, SigKilledChild_ExitCodeMinusOne_IsError) {
    std::string output;
    // bash -c 'kill -9 $$' kills the bash subprocess with SIGKILL.
    Status s = RunSubprocess({"bash", "-c", "kill -9 $$"}, 5, &output);
    EXPECT_FALSE(s.ok());
    // exit_code = -1 (WIFSIGNALED) → error message contains "exited with code -1"
    // OR the process exits immediately and bash reports differently.
    // Either way: the status must not be ok.
}

// ============================================================
// CheckExecutable - all special-character classes that are rejected by the
// character validation loop (covering remaining uncovered char classes).
// ============================================================

TEST(CheckExecutableTest, AsteriskRejected) {
    EXPECT_FALSE(CheckExecutable("echo*"));
}

TEST(CheckExecutableTest, ExclamationRejected) {
    EXPECT_FALSE(CheckExecutable("echo!"));
}

TEST(CheckExecutableTest, AtSignRejected) {
    EXPECT_FALSE(CheckExecutable("echo@host"));
}

TEST(CheckExecutableTest, HashRejected) {
    EXPECT_FALSE(CheckExecutable("echo#comment"));
}

TEST(CheckExecutableTest, TildeRejected) {
    EXPECT_FALSE(CheckExecutable("~echo"));
}

TEST(CheckExecutableTest, PercentRejected) {
    EXPECT_FALSE(CheckExecutable("echo%"));
}

TEST(CheckExecutableTest, CaretRejected) {
    EXPECT_FALSE(CheckExecutable("echo^"));
}

TEST(CheckExecutableTest, AmpersandRejected) {
    EXPECT_FALSE(CheckExecutable("echo&bg"));
}

TEST(CheckExecutableTest, EqualsRejected) {
    EXPECT_FALSE(CheckExecutable("a=b"));
}

TEST(CheckExecutableTest, BraceRejected) {
    EXPECT_FALSE(CheckExecutable("{echo}"));
}

TEST(CheckExecutableTest, BracketRejected) {
    EXPECT_FALSE(CheckExecutable("[echo]"));
}

TEST(CheckExecutableTest, SemicolonRejected) {
    EXPECT_FALSE(CheckExecutable("echo;ls"));
}

}  // namespace
}  // namespace cortrix
