#include <cstdint>
#include "cortrix/connector/file_watcher.h"

#if defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <thread>
#include <atomic>
#include <filesystem>

namespace cortrix {

class FSEventsWatcher : public FileWatcher {
public:
    explicit FSEventsWatcher(double latency_s = 0.5)
        : latency_s_(latency_s) {}

    ~FSEventsWatcher() override {
        Stop();
    }

    int Init(const std::string& root_path,
             FileEventCallback callback) override {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(root_path, ec) || !fs::is_directory(root_path, ec)) {
            return -1;
        }

        // Check read permission
        (void)fs::status(root_path, ec);
        if (ec) return -2;

        root_path_ = fs::canonical(root_path, ec).string();
        if (ec) return -1;

        callback_ = std::move(callback);
        initialized_ = true;
        return 0;
    }

    int Start() override {
        if (!initialized_ || running_.load()) return -1;

        CFStringRef path_ref = CFStringCreateWithCString(
            kCFAllocatorDefault, root_path_.c_str(), kCFStringEncodingUTF8);
        CFArrayRef paths = CFArrayCreate(
            kCFAllocatorDefault, (const void**)&path_ref, 1, &kCFTypeArrayCallBacks);

        FSEventStreamContext ctx = {0, this, nullptr, nullptr, nullptr};

        stream_ = FSEventStreamCreate(
            kCFAllocatorDefault,
            &FSEventsWatcher::FSEventsCallback,
            &ctx,
            paths,
            kFSEventStreamEventIdSinceNow,
            latency_s_,
            kFSEventStreamCreateFlagFileEvents |
            kFSEventStreamCreateFlagUseCFTypes);

        CFRelease(paths);
        CFRelease(path_ref);

        if (!stream_) return -1;

        dispatch_queue_ = dispatch_queue_create(
            "cortrix.fsevents", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(stream_, dispatch_queue_);

        if (!FSEventStreamStart(stream_)) {
            FSEventStreamInvalidate(stream_);
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
            dispatch_release(dispatch_queue_);
            dispatch_queue_ = nullptr;
            return -1;
        }

        running_ = true;
        return 0;
    }

    void Stop() override {
        if (!running_.load()) return;
        running_ = false;

        if (stream_) {
            FSEventStreamStop(stream_);
            FSEventStreamInvalidate(stream_);
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
        }
        if (dispatch_queue_) {
            // Drain the queue before returning: Invalidate stops NEW callbacks
            // but a callback already executing on the queue keeps running —
            // returning from Stop() while it still touches the user callback
            // (and whatever that captures) is a teardown race (TSAN-caught).
            dispatch_sync_f(dispatch_queue_, nullptr, [](void*) {});
            dispatch_release(dispatch_queue_);
            dispatch_queue_ = nullptr;
        }
    }

    bool IsRunning() const override {
        return running_.load();
    }

private:
    static void FSEventsCallback(
        ConstFSEventStreamRef /*stream*/,
        void* context,
        size_t num_events,
        void* event_paths,
        const FSEventStreamEventFlags event_flags[],
        const FSEventStreamEventId /*event_ids*/[]) {

        auto* self = static_cast<FSEventsWatcher*>(context);
        if (!self->running_.load()) return;

        CFArrayRef paths_array = static_cast<CFArrayRef>(event_paths);
        std::vector<FileEvent> events;
        events.reserve(num_events);

        for (size_t i = 0; i < num_events; ++i) {
            CFStringRef cf_path = static_cast<CFStringRef>(
                CFArrayGetValueAtIndex(paths_array, static_cast<CFIndex>(i)));
            char path_buf[PATH_MAX];
            if (!CFStringGetCString(cf_path, path_buf, sizeof(path_buf),
                                     kCFStringEncodingUTF8)) {
                continue;
            }

            std::string path(path_buf);
            FSEventStreamEventFlags flags = event_flags[i];

            FileEvent ev;
            ev.path = path;
            ev.is_directory = (flags & kFSEventStreamEventFlagItemIsDir) != 0;

            auto now = std::chrono::system_clock::now();
            ev.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();

            if (flags & kFSEventStreamEventFlagItemRenamed) {
                namespace fs = std::filesystem;
                std::error_code ec;
                if (fs::exists(path, ec)) {
                    ev.type = FileEventType::kCreated;
                } else {
                    ev.type = FileEventType::kDeleted;
                }
                events.push_back(std::move(ev));
            } else if (flags & kFSEventStreamEventFlagItemRemoved) {
                ev.type = FileEventType::kDeleted;
                events.push_back(std::move(ev));
            } else if (flags & kFSEventStreamEventFlagItemCreated) {
                ev.type = FileEventType::kCreated;
                events.push_back(std::move(ev));
            } else if (flags & kFSEventStreamEventFlagItemModified) {
                ev.type = FileEventType::kModified;
                events.push_back(std::move(ev));
            }
        }

        if (!events.empty() && self->callback_) {
            self->callback_(events);
        }
    }

    std::string root_path_;
    FileEventCallback callback_;
    double latency_s_;
    bool initialized_ = false;

    FSEventStreamRef stream_ = nullptr;
    dispatch_queue_t dispatch_queue_ = nullptr;
    std::atomic<bool> running_{false};
};

std::unique_ptr<FileWatcher> FileWatcher::Create() {
    return std::make_unique<FSEventsWatcher>();
}

}  // namespace cortrix

#elif defined(__linux__)

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <filesystem>

namespace cortrix {

class InotifyWatcher : public FileWatcher {
public:
    InotifyWatcher() = default;

    ~InotifyWatcher() override {
        Stop();
        if (inotify_fd_ >= 0) close(inotify_fd_);
        if (pipe_fd_[0] >= 0) close(pipe_fd_[0]);
        if (pipe_fd_[1] >= 0) close(pipe_fd_[1]);
    }

    int Init(const std::string& root_path,
             FileEventCallback callback) override {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(root_path, ec) || !fs::is_directory(root_path, ec)) {
            return -1;
        }

        root_path_ = fs::canonical(root_path, ec).string();
        if (ec) return -1;
        callback_ = std::move(callback);

        inotify_fd_ = inotify_init1(IN_NONBLOCK);
        if (inotify_fd_ < 0) return -1;

        if (pipe(pipe_fd_) < 0) return -1;

        if (AddWatchRecursive(root_path_) < 0) return -1;

        return 0;
    }

    int Start() override {
        if (running_.load()) return -1;
        running_ = true;
        watch_thread_ = std::thread([this]() { EventLoop(); });
        return 0;
    }

    void Stop() override {
        if (!running_.load()) return;
        running_ = false;
        // Signal the poll to wake up
        char c = 1;
        (void)write(pipe_fd_[1], &c, 1);
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
    }

    bool IsRunning() const override {
        return running_.load();
    }

private:
    int AddWatchRecursive(const std::string& dir_path) {
        uint32_t mask = IN_CREATE | IN_MODIFY | IN_DELETE |
                        IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF;
        int wd = inotify_add_watch(inotify_fd_, dir_path.c_str(), mask);
        if (wd < 0) return -1;

        {
            std::lock_guard<std::mutex> lock(wd_mu_);
            wd_to_path_[wd] = dir_path;
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(dir_path, ec)) {
            if (entry.is_directory(ec)) {
                AddWatchRecursive(entry.path().string());
            }
        }
        return 0;
    }

    void EventLoop() {
        struct pollfd fds[2];
        fds[0].fd = inotify_fd_;
        fds[0].events = POLLIN;
        fds[1].fd = pipe_fd_[0];
        fds[1].events = POLLIN;

        char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

        while (running_.load()) {
            int ret = poll(fds, 2, 1000);
            if (ret <= 0) continue;
            if (fds[1].revents & POLLIN) break;  // stop signal

            if (!(fds[0].revents & POLLIN)) continue;

            ssize_t len = read(inotify_fd_, buf, sizeof(buf));
            if (len <= 0) continue;

            std::vector<FileEvent> events;
            const char* ptr = buf;
            while (ptr < buf + len) {
                auto* ev = reinterpret_cast<const struct inotify_event*>(ptr);
                if (ev->len > 0) {
                    std::string dir_path;
                    {
                        std::lock_guard<std::mutex> lock(wd_mu_);
                        auto it = wd_to_path_.find(ev->wd);
                        if (it != wd_to_path_.end()) dir_path = it->second;
                    }
                    if (!dir_path.empty()) {
                        std::string full_path = dir_path + "/" + ev->name;
                        FileEvent fe;
                        fe.path = full_path;
                        fe.is_directory = (ev->mask & IN_ISDIR) != 0;
                        auto now = std::chrono::system_clock::now();
                        fe.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch()).count();

                        if (ev->mask & IN_CREATE) {
                            fe.type = FileEventType::kCreated;
                            if (fe.is_directory) AddWatchRecursive(full_path);
                        } else if (ev->mask & IN_MODIFY) {
                            fe.type = FileEventType::kModified;
                        } else if (ev->mask & IN_DELETE) {
                            fe.type = FileEventType::kDeleted;
                        } else if (ev->mask & IN_MOVED_FROM) {
                            fe.type = FileEventType::kDeleted;
                        } else if (ev->mask & IN_MOVED_TO) {
                            fe.type = FileEventType::kCreated;
                            if (fe.is_directory) AddWatchRecursive(full_path);
                        }
                        events.push_back(std::move(fe));
                    }
                }
                ptr += sizeof(struct inotify_event) + ev->len;
            }

            if (!events.empty() && callback_) {
                callback_(events);
            }
        }
    }

    std::string root_path_;
    FileEventCallback callback_;

    int inotify_fd_ = -1;
    int pipe_fd_[2] = {-1, -1};

    std::unordered_map<int, std::string> wd_to_path_;
    std::mutex wd_mu_;

    std::thread watch_thread_;
    std::atomic<bool> running_{false};
};

std::unique_ptr<FileWatcher> FileWatcher::Create() {
    return std::make_unique<InotifyWatcher>();
}

}  // namespace cortrix

#else
#error "Unsupported platform: F05 requires macOS or Linux"
#endif
