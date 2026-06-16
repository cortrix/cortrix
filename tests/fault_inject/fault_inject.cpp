// F23 §4.6 syscall fault-injection seam — macOS dyld interpose impl.
// See fault_inject.h for the contract and safety interlocks.
#include "fault_inject.h"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

namespace {

enum class Op : int {
  kNone = 0,
  kWrite,
  kPwrite,
  kPread,
  kRead,
  kFsync,
  kFullFsync,
  kOpen,
  kRename,
  kFtruncate,
};

Op ParseOp(const char* s) {
  if (!s) return Op::kNone;
  if (std::strcmp(s, "write") == 0) return Op::kWrite;
  if (std::strcmp(s, "pwrite") == 0) return Op::kPwrite;
  if (std::strcmp(s, "pread") == 0) return Op::kPread;
  if (std::strcmp(s, "read") == 0) return Op::kRead;
  if (std::strcmp(s, "fsync") == 0) return Op::kFsync;
  if (std::strcmp(s, "fullfsync") == 0) return Op::kFullFsync;
  if (std::strcmp(s, "open") == 0) return Op::kOpen;
  if (std::strcmp(s, "rename") == 0) return Op::kRename;
  if (std::strcmp(s, "ftruncate") == 0) return Op::kFtruncate;
  return Op::kNone;
}

// `g_active` is the master gate: false (the default, and the state during
// gtest discovery + the vast majority of tests) makes EVERY interposed call
// a pure pass-through after a single relaxed load — no mutex, no fd-table
// touch. This matters for correctness as much as speed: open/close are
// interposed process-wide and fire during very early process startup
// (dyld, libc++ init) before the function-local mutex/map singletons would
// otherwise need to exist. Only FailOp() flips it on.
std::atomic<bool> g_active{false};

// Armed config. `op == kNone` (the common, disarmed state) short-circuits
// every interposed call after one relaxed load.
std::atomic<int> g_op{static_cast<int>(Op::kNone)};
std::atomic<int> g_skip{0};
std::atomic<int> g_count{0};
std::atomic<int> g_err{0};
std::atomic<int> g_matched{0};
std::atomic<int> g_consumed{0};

// path_substr + the fd->path table share one mutex; all are cold paths
// (open/close/arm) or armed-only paths, never the disarmed fast path of
// read/write/fsync.
std::mutex& TableMu() {
  static auto* mu = new std::mutex();
  return *mu;
}
std::string& PathSubstr() {
  static auto* s = new std::string();
  return *s;
}
std::unordered_map<int, std::string>& FdTable() {
  static auto* t = new std::unordered_map<int, std::string>();
  return *t;
}

bool Armed(Op op) {
  return g_active.load(std::memory_order_relaxed) &&
         g_op.load(std::memory_order_relaxed) == static_cast<int>(op);
}

// skip/count window. Returns true if THIS call must fail.
bool ConsumeWindow() {
  g_matched.fetch_add(1, std::memory_order_relaxed);
  int s = g_skip.load(std::memory_order_relaxed);
  while (s > 0) {
    if (g_skip.compare_exchange_weak(s, s - 1, std::memory_order_relaxed))
      return false;
  }
  int c = g_count.load(std::memory_order_relaxed);
  while (c > 0) {
    if (g_count.compare_exchange_weak(c, c - 1, std::memory_order_relaxed)) {
      g_consumed.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

bool PathMatches(const char* path) {
  std::lock_guard<std::mutex> lk(TableMu());
  const std::string& ps = PathSubstr();
  return ps.empty() || (path && std::strstr(path, ps.c_str()) != nullptr);
}

// fd-based ops: never std streams; path filter resolves via the fd table.
bool ShouldFailFd(Op op, int fd) {
  if (!Armed(op)) return false;
  if (fd >= 0 && fd <= 2) return false;
  {
    std::lock_guard<std::mutex> lk(TableMu());
    const std::string& ps = PathSubstr();
    if (!ps.empty()) {
      auto it = FdTable().find(fd);
      if (it == FdTable().end() ||
          it->second.find(ps) == std::string::npos)
        return false;
    }
  }
  return ConsumeWindow();
}

// path-based ops (open/rename).
bool ShouldFailPath(Op op, const char* path) {
  if (!Armed(op)) return false;
  if (!PathMatches(path)) return false;
  return ConsumeWindow();
}

void RecordFd(int fd, const char* path) {
  if (fd < 0 || !path) return;
  std::lock_guard<std::mutex> lk(TableMu());
  FdTable()[fd] = path;
}

void ForgetFd(int fd) {
  std::lock_guard<std::mutex> lk(TableMu());
  FdTable().erase(fd);
}

// ---- interposed replacements --------------------------------------
// Inside this dylib, calls to the plain libc names still bind to the
// real implementations (dyld never applies an image's interposing to
// the image itself), so the pass-through calls below are not recursive.

int my_open(const char* path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = static_cast<mode_t>(va_arg(ap, int));
    va_end(ap);
  }
  if (!g_active.load(std::memory_order_relaxed))
    return (flags & O_CREAT) ? open(path, flags, mode) : open(path, flags);
  if (ShouldFailPath(Op::kOpen, path)) {
    errno = g_err.load(std::memory_order_relaxed);
    return -1;
  }
  int fd = (flags & O_CREAT) ? open(path, flags, mode) : open(path, flags);
  if (fd >= 0) RecordFd(fd, path);
  return fd;
}

int my_close(int fd) {
  if (g_active.load(std::memory_order_relaxed)) ForgetFd(fd);
  return close(fd);
}

ssize_t my_write(int fd, const void* buf, size_t n) {
  if (ShouldFailFd(Op::kWrite, fd)) {
    errno = g_err.load(std::memory_order_relaxed);
    return -1;
  }
  return write(fd, buf, n);
}

ssize_t my_pwrite(int fd, const void* buf, size_t n, off_t off) {
  if (ShouldFailFd(Op::kPwrite, fd)) {
    errno = g_err.load(std::memory_order_relaxed);
    return -1;
  }
  return pwrite(fd, buf, n, off);
}

ssize_t my_read(int fd, void* buf, size_t n) {
  if (ShouldFailFd(Op::kRead, fd)) {
    errno = g_err.load(std::memory_order_relaxed);
    return -1;
  }
  return read(fd, buf, n);
}

ssize_t my_pread(int fd, void* buf, size_t n, off_t off) {
  if (ShouldFailFd(Op::kPread, fd)) {
    errno = g_err.load(std::memory_order_relaxed);
    return -1;
  }
  return pread(fd, buf, n, off);
}

int my_fsync(int fd) {
  if (ShouldFailFd(Op::kFsync, fd)) {
    errno = g_err.load(std::memory_order_relaxed);
    return -1;
  }
  return fsync(fd);
}

int my_ftruncate(int fd, off_t len) {
  if (ShouldFailFd(Op::kFtruncate, fd)) {
    errno = g_err.load(std::memory_order_relaxed);
    return -1;
  }
  return ftruncate(fd, len);
}

int my_rename(const char* from, const char* to) {
  if (Armed(Op::kRename) && (PathMatches(from) || PathMatches(to)) &&
      ConsumeWindow()) {
    errno = g_err.load(std::memory_order_relaxed);
    return -1;
  }
  return rename(from, to);
}

// fcntl is variadic: forward one pointer-sized slot unconditionally.
// Promoted int args occupy a full slot in the Apple arm64/x86-64 varargs
// ABI, so the bit pattern survives the round trip for every cmd.
int my_fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  void* arg = va_arg(ap, void*);
  va_end(ap);
#ifdef F_FULLFSYNC
  if (cmd == F_FULLFSYNC && ShouldFailFd(Op::kFullFsync, fd)) {
#else
  if (false && ShouldFailFd(Op::kFullFsync, fd)) {
#endif
    errno = g_err.load(std::memory_order_relaxed);
    return -1;
  }
  return fcntl(fd, cmd, arg);
}

// ---- dyld interpose table ------------------------------------------
struct interpose_s {
  const void* replacement;
  const void* original;
};

#define CORTRIX_IP(repl, orig)                                     \
  { reinterpret_cast<const void*>(&repl),                          \
    reinterpret_cast<const void*>(&orig) }

__attribute__((used)) static const interpose_s kInterposers[]
    __attribute__((section("__DATA,__interpose"))) = {
        CORTRIX_IP(my_open, open),
        CORTRIX_IP(my_close, close),
        CORTRIX_IP(my_write, write),
        CORTRIX_IP(my_pwrite, pwrite),
        CORTRIX_IP(my_read, read),
        CORTRIX_IP(my_pread, pread),
        CORTRIX_IP(my_fsync, fsync),
        CORTRIX_IP(my_ftruncate, ftruncate),
        CORTRIX_IP(my_rename, rename),
        CORTRIX_IP(my_fcntl, fcntl),
};

}  // namespace

namespace cortrix::fi {

void FailOp(const char* op, int skip, int count, int err,
            const char* path_substr) {
  // Disarm first so no interposed call consumes a half-written config.
  g_op.store(static_cast<int>(Op::kNone), std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(TableMu());
    PathSubstr() = path_substr ? path_substr : "";
  }
  g_skip.store(skip, std::memory_order_relaxed);
  g_count.store(count, std::memory_order_relaxed);
  g_err.store(err, std::memory_order_relaxed);
  g_matched.store(0, std::memory_order_relaxed);
  g_consumed.store(0, std::memory_order_relaxed);
  g_op.store(static_cast<int>(ParseOp(op)), std::memory_order_relaxed);
  g_active.store(true, std::memory_order_release);
}

void Disarm() {
  g_active.store(false, std::memory_order_relaxed);
  g_op.store(static_cast<int>(Op::kNone), std::memory_order_relaxed);
}

int ConsumedCount() { return g_consumed.load(std::memory_order_relaxed); }

int MatchedCount() { return g_matched.load(std::memory_order_relaxed); }

}  // namespace cortrix::fi
