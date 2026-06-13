#pragma once
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>

namespace cortrix {

enum class FileEventType {
    kCreated  = 0,
    kModified = 1,
    kDeleted  = 2,
    kRenamed  = 3,   // treated as delete old + create new
};

struct FileEvent {
    FileEventType type;
    std::string path;               // absolute path
    int64_t timestamp_us = 0;       // event time (microseconds since epoch)
    bool is_directory = false;
};

/// Batch event callback to reduce lock contention
using FileEventCallback = std::function<void(const std::vector<FileEvent>& events)>;

class FileWatcher {
public:
    virtual ~FileWatcher() = default;

    /// Initialize watcher
    /// @param root_path: monitored root directory (absolute path)
    /// @param callback: file event callback
    /// @return 0 = success, -1 = directory not found, -2 = permission denied
    virtual int Init(const std::string& root_path,
                     FileEventCallback callback) = 0;

    /// Start watching (blocking or internal thread)
    /// @return 0 = success, -1 = already started / init incomplete
    virtual int Start() = 0;

    /// Stop watching
    virtual void Stop() = 0;

    /// Check if running
    virtual bool IsRunning() const = 0;

    /// Factory method: create platform-specific implementation
    static std::unique_ptr<FileWatcher> Create();
};

}  // namespace cortrix
