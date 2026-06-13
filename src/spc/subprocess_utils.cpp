#include "cortrix/spc/subprocess_utils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <chrono>

namespace cortrix {

Status RunSubprocess(const std::vector<std::string>& args, int timeout_s, std::string* output) {
    if (args.empty()) {
        return Status::InvalidArgument("empty args");
    }
    if (!output) {
        return Status::InvalidArgument("null output pointer");
    }
    output->clear();

    // Create a pipe for capturing child stdout
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        return Status::Internal("pipe() failed: " + std::string(strerror(errno)));
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return Status::Internal("fork() failed: " + std::string(strerror(errno)));
    }

    if (pid == 0) {
        // Child process
        close(pipe_fd[0]);  // Close read end
        dup2(pipe_fd[1], STDOUT_FILENO);  // Redirect stdout to pipe
        close(pipe_fd[1]);

        // Build argv array for execvp
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        // If execvp returns, it failed
        _exit(127);
    }

    // Parent process
    close(pipe_fd[1]);  // Close write end

    // Set pipe read end to non-blocking for timeout support
    int flags = fcntl(pipe_fd[0], F_GETFL, 0);
    if (flags != -1) {
        fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);
    }

    // Use timeout_s to enforce deadline; <= 0 means no timeout
    auto deadline = std::chrono::steady_clock::now();
    bool has_timeout = (timeout_s > 0);
    if (has_timeout) {
        deadline += std::chrono::seconds(timeout_s);
    }

    bool timed_out = false;

    // Read child stdout with timeout via poll()
    struct pollfd pfd;
    pfd.fd = pipe_fd[0];
    pfd.events = POLLIN;

    for (;;) {
        int poll_timeout_ms = -1; // infinite
        if (has_timeout) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                timed_out = true;
                break;
            }
            poll_timeout_ms = static_cast<int>(remaining.count());
        }

        int poll_rc = poll(&pfd, 1, poll_timeout_ms);
        if (poll_rc == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (poll_rc == 0) {
            // Timeout expired
            timed_out = true;
            break;
        }

        // Data available
        std::array<char, 4096> buffer;
        ssize_t bytes_read = read(pipe_fd[0], buffer.data(), buffer.size());
        if (bytes_read > 0) {
            output->append(buffer.data(), static_cast<size_t>(bytes_read));
        } else if (bytes_read == 0) {
            // EOF - child closed stdout
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR) continue;
            break;
        }
    }

    close(pipe_fd[0]);

    if (timed_out) {
        // Kill the child process group
        kill(pid, SIGKILL);
        // Reap the zombie
        int status;
        waitpid(pid, &status, 0);
        return Status::Internal("subprocess timed out after " +
                                std::to_string(timeout_s) + "s");
    }

    // Wait for child to finish
    int status;
    if (waitpid(pid, &status, 0) == -1) {
        return Status::Internal("waitpid() failed: " + std::string(strerror(errno)));
    }

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exit_code != 0) {
        return Status::Internal("subprocess exited with code " +
                                std::to_string(exit_code));
    }

    return Status::Ok();
}

bool CheckExecutable(const std::string& executable) {
    if (executable.empty()) {
        return false;
    }

    // Validate executable name contains no path separators or shell metacharacters.
    // We only accept simple command names (alphanumeric, dash, underscore, dot).
    for (char c : executable) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) ||
              c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }

    // Use RunSubprocess with argv to avoid shell injection
    std::vector<std::string> args = {"which", executable};
    std::string which_output;
    Status s = RunSubprocess(args, 5, &which_output);
    return s.ok() && !which_output.empty();
}

}  // namespace cortrix
