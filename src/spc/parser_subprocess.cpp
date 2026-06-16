#include <cstdint>
#include "cortrix/spc/parser_subprocess.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace cortrix::spc {

namespace {

void SetNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags != -1) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Drain a ready fd into `sink` (capped). Returns false on EOF, true otherwise.
// Sets `overflow` if the cap was hit.
bool DrainFd(int fd, std::string* sink, int64_t cap, bool* overflow) {
    std::array<char, 4096> buf;
    for (;;) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) {
            if (cap > 0 && static_cast<int64_t>(sink->size()) + n > cap) {
                const int64_t room = cap - static_cast<int64_t>(sink->size());
                if (room > 0) sink->append(buf.data(), static_cast<size_t>(room));
                *overflow = true;
                return true;  // caller will kill the child
            }
            sink->append(buf.data(), static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return false;  // EOF
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  // no more now
        if (errno == EINTR) continue;
        return false;  // hard read error → treat like EOF
    }
}

}  // namespace

SubprocessResult RunParserSubprocess(const std::vector<std::string>& args,
                                     int timeout_ms,
                                     int64_t max_output_bytes,
                                     int kill_grace_ms) {
    SubprocessResult res;
    if (args.empty()) return res;  // launched=false

    int out_pipe[2];
    int err_pipe[2];
    if (::pipe(out_pipe) == -1) return res;
    if (::pipe(err_pipe) == -1) {
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        return res;
    }

    // posix_spawnp, NOT fork+execvp: the server maps the ONNX models (20GB+
    // of address space), and fork's page-table copy gets refused with ENOMEM
    // on small hosts (hit live in the 8GB container VM). posix_spawn uses
    // vfork semantics — no address-space copy — and reports launch errors
    // (including PATH lookup failure) as its return value.
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    ::posix_spawn_file_actions_init(&fa);
    ::posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
    ::posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
    ::posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
    ::posix_spawn_file_actions_addclose(&fa, err_pipe[1]);
    ::posix_spawnattr_init(&attr);
    // Own process group so a timeout kill can't be dodged by grandchildren.
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    ::posix_spawnattr_setpgroup(&attr, 0);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    int spawn_errno = ::posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(),
                                     environ);
    ::posix_spawn_file_actions_destroy(&fa);
    ::posix_spawnattr_destroy(&attr);

    if (spawn_errno != 0) {
        // A silent launched=false here surfaced upstream only as the opaque
        // "All parsers failed" — keep the errno visible.
        spdlog::warn("parser subprocess spawn failed: argv0='{}' errno={} ({})",
                     args[0], spawn_errno, std::strerror(spawn_errno));
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        return res;  // launched=false
    }

    // Parent.
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    res.launched = true;
    SetNonBlocking(out_pipe[0]);
    SetNonBlocking(err_pipe[0]);

    const bool has_timeout = (timeout_ms > 0);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(has_timeout ? timeout_ms : 0);

    bool out_open = true, err_open = true;
    bool must_kill = false;

    while (out_open || err_open) {
        struct pollfd pfds[2];
        int nfds = 0;
        int out_idx = -1, err_idx = -1;
        if (out_open) { pfds[nfds] = {out_pipe[0], POLLIN, 0}; out_idx = nfds++; }
        if (err_open) { pfds[nfds] = {err_pipe[0], POLLIN, 0}; err_idx = nfds++; }

        int poll_ms = -1;
        if (has_timeout) {
            auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (rem.count() <= 0) { res.timed_out = true; must_kill = true; break; }
            poll_ms = static_cast<int>(rem.count());
        }

        int rc = ::poll(pfds, nfds, poll_ms);
        if (rc == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) { res.timed_out = true; must_kill = true; break; }

        if (out_idx >= 0 && (pfds[out_idx].revents & (POLLIN | POLLHUP))) {
            if (!DrainFd(out_pipe[0], &res.stdout_data, max_output_bytes,
                         &res.output_truncated)) {
                out_open = false;
            }
            if (res.output_truncated) { must_kill = true; break; }
        }
        if (err_idx >= 0 && (pfds[err_idx].revents & (POLLIN | POLLHUP))) {
            // Cap stderr to a sane tail (64KB) so a chatty child can't OOM us.
            bool unused = false;
            if (!DrainFd(err_pipe[0], &res.stderr_data, 64 * 1024, &unused)) {
                err_open = false;
            }
        }
    }

    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    if (must_kill) {
        // SIGTERM → grace → SIGKILL (§3.3). Signal the whole process group.
        ::kill(-pid, SIGTERM);
        const auto grace_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kill_grace_ms);
        int status = 0;
        while (std::chrono::steady_clock::now() < grace_deadline) {
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                // Exited within grace.
                return res;  // timed_out / output_truncated already set
            }
            ::usleep(20 * 1000);
        }
        ::kill(-pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        return res;
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) == -1) {
        // Could not reap; report as a failed launch outcome.
        res.exit_code = -1;
        return res;
    }
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return res;
}

}  // namespace cortrix::spc
