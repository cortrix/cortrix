#pragma once
// =============================================================
// Test suite syscall fault-injection framework (TEST-ONLY).
//
// Built as a separate shared library (cortrix_fault_inject) that the
// unit-test binary links; it uses macOS dyld interposing
// (__DATA,__interpose) to wrap the POSIX I/O syscall wrappers
// (write/pwrite/pread/read/fsync/fcntl(F_FULLFSYNC)/open/rename/
// ftruncate/close) process-wide. Production binaries never link this
// library, so production carries zero overhead and zero risk.
//
// Why: the durability layers (write coordinator pending log, index P-HNSW WAL +
// snapshots) implement graceful handling for every syscall failure
// (EIO, ENOSPC, short writes...). Those error branches are physically
// unreachable from user-space tests against a healthy filesystem;
// this seam makes them reachable (g1/g2 branch-gap lists, ~330 arms).
//
// Usage pattern (fault sweep — systematically hits EVERY call site):
//   for (int k = 0;; ++k) {
//     fi::FailOp("pwrite", /*skip=*/k, /*count=*/1, EIO, "pending.log");
//     int rc = RunWorkload();
//     bool consumed = fi::ConsumedCount() > 0;
//     fi::Disarm();
//     if (!consumed) break;        // k is past the last pwrite -> swept all
//     EXPECT_NE(rc, 0);            // workload must fail gracefully
//     VerifyRecoverable();         // reopen/recover must succeed
//   }
//
// Safety interlocks:
//   - fds 0/1/2 are never injected (gtest/stdio traffic).
//   - A path substring filter scopes injection to the file under test
//     (fd-based ops resolve the path via an open()-maintained fd table),
//     so SQLite / gcov / gtest internal I/O is never affected.
//   - Always Disarm() in TearDown; the armed window should only span
//     the workload under test.
// =============================================================

namespace cortrix::fi {

// Arm the seam: after skipping `skip` matching calls, fail the next
// `count` matching calls with errno = `err` (return -1).
//   op:   "write" | "pwrite" | "pread" | "read" | "fsync" |
//         "fullfsync" (fcntl F_FULLFSYNC) | "open" | "rename" |
//         "ftruncate"
//   path_substr: only calls whose target path contains this substring
//         match (nullptr/"" = match any non-std fd). For fd-based ops
//         the path recorded at open() time is used; fds opened before
//         this library loaded never match.
// Calling FailOp replaces any previous armed config.
void FailOp(const char* op, int skip, int count, int err,
            const char* path_substr);

// Disarm and forget any armed config (idempotent).
void Disarm();

// Number of calls actually failed since the last FailOp().
int ConsumedCount();

// Number of calls that MATCHED (op + path + non-std fd) since the last
// FailOp(), including skipped and failed ones. Lets a sweep detect
// "skip is past the end" (matched == skipped, consumed == 0).
int MatchedCount();

}  // namespace cortrix::fi
