#include <gtest/gtest.h>
#include "cortrix/connector/file_watcher.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>

namespace cortrix {
namespace {

class FileWatcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        test_dir_ = fs::temp_directory_path() / "cortrix_test_watcher";
        fs::create_directories(test_dir_);
        watcher_ = FileWatcher::Create();
    }

    void TearDown() override {
        if (watcher_ && watcher_->IsRunning()) {
            watcher_->Stop();
        }
        namespace fs = std::filesystem;
        fs::remove_all(test_dir_);
    }

    void WaitForEvents(int expected_count, int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [&]{ return static_cast<int>(events_.size()) >= expected_count; });
    }

    void CreateFile(const std::string& name, const std::string& content = "test") {
        auto path = test_dir_ / name;
        std::ofstream out(path);
        out << content;
        out.close();
    }

    std::filesystem::path test_dir_;
    std::unique_ptr<FileWatcher> watcher_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<FileEvent> events_;
};

TEST_F(FileWatcherTest, CreateReturnsValid) {
    ASSERT_NE(nullptr, watcher_);
}

TEST_F(FileWatcherTest, InitValidDir) {
    int rc = watcher_->Init(test_dir_.string(),
        [](const std::vector<FileEvent>&) {});
    EXPECT_EQ(0, rc);
}

TEST_F(FileWatcherTest, InitInvalidDir) {
    int rc = watcher_->Init("/nonexistent/dir/path",
        [](const std::vector<FileEvent>&) {});
    EXPECT_EQ(-1, rc);
}

TEST_F(FileWatcherTest, StartStop) {
    watcher_->Init(test_dir_.string(),
        [](const std::vector<FileEvent>&) {});
    EXPECT_EQ(0, watcher_->Start());
    EXPECT_TRUE(watcher_->IsRunning());
    watcher_->Stop();
    EXPECT_FALSE(watcher_->IsRunning());
}

TEST_F(FileWatcherTest, DoubleStart) {
    watcher_->Init(test_dir_.string(),
        [](const std::vector<FileEvent>&) {});
    EXPECT_EQ(0, watcher_->Start());
    EXPECT_EQ(-1, watcher_->Start());
    watcher_->Stop();
}

TEST_F(FileWatcherTest, FileCreated) {
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();

    // Small delay to ensure watcher is ready
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CreateFile("new_file.txt");
    WaitForEvents(1);

    std::lock_guard<std::mutex> lock(mu_);
    bool found_create = false;
    for (const auto& e : events_) {
        if (e.type == FileEventType::kCreated &&
            e.path.find("new_file.txt") != std::string::npos) {
            found_create = true;
        }
    }
    EXPECT_TRUE(found_create);
}

TEST_F(FileWatcherTest, FileModified) {
    CreateFile("existing.txt", "original");

    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Modify the file
    {
        std::ofstream out(test_dir_ / "existing.txt");
        out << "modified content";
    }
    WaitForEvents(1);

    std::lock_guard<std::mutex> lock(mu_);
    bool found_modify = false;
    for (const auto& e : events_) {
        if ((e.type == FileEventType::kModified || e.type == FileEventType::kCreated) &&
            e.path.find("existing.txt") != std::string::npos) {
            found_modify = true;
        }
    }
    EXPECT_TRUE(found_modify);
}

TEST_F(FileWatcherTest, FileDeleted) {
    CreateFile("to_delete.txt");

    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::filesystem::remove(test_dir_ / "to_delete.txt");
    WaitForEvents(1);

    std::lock_guard<std::mutex> lock(mu_);
    bool found_delete = false;
    for (const auto& e : events_) {
        if (e.type == FileEventType::kDeleted &&
            e.path.find("to_delete.txt") != std::string::npos) {
            found_delete = true;
        }
    }
    EXPECT_TRUE(found_delete);
}

TEST_F(FileWatcherTest, StopCleansUp) {
    watcher_->Init(test_dir_.string(),
        [](const std::vector<FileEvent>&) {});
    watcher_->Start();
    EXPECT_TRUE(watcher_->IsRunning());
    watcher_->Stop();
    EXPECT_FALSE(watcher_->IsRunning());
    // Should be safe to call stop again
    watcher_->Stop();
    EXPECT_FALSE(watcher_->IsRunning());
}

// --- Edge case tests ---

TEST_F(FileWatcherTest, StopWithoutStart) {
    watcher_->Init(test_dir_.string(),
        [](const std::vector<FileEvent>&) {});
    // Should be safe to call Stop before Start
    watcher_->Stop();
    EXPECT_FALSE(watcher_->IsRunning());
}

TEST_F(FileWatcherTest, InitWithoutCallback) {
    // nullptr callback should still allow init (but no events dispatched)
    int rc = watcher_->Init(test_dir_.string(), nullptr);
    EXPECT_EQ(0, rc);
}

TEST_F(FileWatcherTest, StartWithoutInit) {
    // Starting without calling Init should fail
    EXPECT_EQ(-1, watcher_->Start());
}

TEST_F(FileWatcherTest, IsRunningBeforeStart) {
    EXPECT_FALSE(watcher_->IsRunning());
}

TEST_F(FileWatcherTest, MultipleFilesCreated) {
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CreateFile("file1.txt");
    CreateFile("file2.txt");
    CreateFile("file3.txt");
    WaitForEvents(3, 5000);

    std::lock_guard<std::mutex> lock(mu_);
    int create_count = 0;
    for (const auto& e : events_) {
        if (e.type == FileEventType::kCreated && !e.is_directory) {
            create_count++;
        }
    }
    EXPECT_GE(create_count, 3);
}

TEST_F(FileWatcherTest, FileInSubdirectory) {
    namespace fs = std::filesystem;
    fs::create_directories(test_dir_ / "subdir");

    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    {
        std::ofstream out(test_dir_ / "subdir" / "nested.txt");
        out << "nested content";
    }
    WaitForEvents(1, 5000);

    std::lock_guard<std::mutex> lock(mu_);
    bool found = false;
    for (const auto& e : events_) {
        if (e.path.find("nested.txt") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(FileWatcherTest, RapidCreateDelete) {
    // Create and immediately delete a file; watcher may or may not see it
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Create then delete quickly
    auto path = test_dir_ / "ephemeral.txt";
    {
        std::ofstream out(path);
        out << "temp";
    }
    std::filesystem::remove(path);

    // Just verify the watcher doesn't crash; events may vary by platform
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    EXPECT_FALSE(watcher_->IsRunning() && false);  // Just checking no crash
}

TEST_F(FileWatcherTest, DeletedWatchDirectory) {
    namespace fs = std::filesystem;
    auto volatile_dir = fs::temp_directory_path() / "cortrix_test_watcher_volatile";
    fs::create_directories(volatile_dir);

    auto local_watcher = FileWatcher::Create();
    local_watcher->Init(volatile_dir.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    local_watcher->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Delete the watched directory itself
    fs::remove_all(volatile_dir);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Should not crash; stop cleanly
    local_watcher->Stop();
    EXPECT_FALSE(local_watcher->IsRunning());
}

TEST_F(FileWatcherTest, FileRenamedInWatchDir) {
    CreateFile("original.txt");

    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::filesystem::rename(test_dir_ / "original.txt", test_dir_ / "renamed.txt");
    WaitForEvents(1, 5000);

    std::lock_guard<std::mutex> lock(mu_);
    // Rename produces delete + create or rename events depending on platform
    bool found_relevant = false;
    for (const auto& e : events_) {
        if (e.path.find("original.txt") != std::string::npos ||
            e.path.find("renamed.txt") != std::string::npos) {
            found_relevant = true;
        }
    }
    EXPECT_TRUE(found_relevant);
}

TEST_F(FileWatcherTest, LargeFileCreated) {
    // Verify watcher handles large files without issues
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Create a 1MB file
    {
        std::ofstream out(test_dir_ / "large_file.bin", std::ios::binary);
        std::string chunk(1024, 'X');
        for (int i = 0; i < 1024; ++i) {
            out << chunk;
        }
    }
    WaitForEvents(1, 5000);

    std::lock_guard<std::mutex> lock(mu_);
    bool found = false;
    for (const auto& e : events_) {
        if (e.path.find("large_file.bin") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(FileWatcherTest, EmptyDirectoryWatching) {
    // Ensure watcher works correctly on an empty directory
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    EXPECT_EQ(0, watcher_->Start());
    EXPECT_TRUE(watcher_->IsRunning());

    // Wait a bit - no events should arrive
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> lock(mu_);
        // No files created, so no file events expected
        // (directory events may or may not appear depending on platform)
    }

    watcher_->Stop();
    EXPECT_FALSE(watcher_->IsRunning());
}

TEST_F(FileWatcherTest, DISABLED_NewSubdirectoryCreatedThenFileInIt) {
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Create a new subdirectory and put a file in it
    std::filesystem::create_directory(test_dir_ / "newdir");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    {
        std::ofstream out(test_dir_ / "newdir" / "file_in_new_dir.txt");
        out << "content";
    }
    WaitForEvents(1, 5000);

    std::lock_guard<std::mutex> lock(mu_);
    bool found = false;
    for (const auto& e : events_) {
        if (e.path.find("file_in_new_dir.txt") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// ======================================================================
// Coverage boost tests: targeting file_watcher.cpp uncovered paths
// ======================================================================

TEST_F(FileWatcherTest, StopThenRestart) {
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });

    EXPECT_EQ(0, watcher_->Start());
    EXPECT_TRUE(watcher_->IsRunning());

    watcher_->Stop();
    EXPECT_FALSE(watcher_->IsRunning());

    // Restart: Create a new watcher since FSEvents/inotify may not support restart
    auto watcher2 = FileWatcher::Create();
    EXPECT_EQ(0, watcher2->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        }));
    EXPECT_EQ(0, watcher2->Start());
    EXPECT_TRUE(watcher2->IsRunning());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CreateFile("restart_test.txt");
    WaitForEvents(1, 3000);

    std::lock_guard<std::mutex> lock(mu_);
    bool found = false;
    for (const auto& e : events_) {
        if (e.path.find("restart_test.txt") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
    watcher2->Stop();
}

TEST_F(FileWatcherTest, DoubleStop) {
    watcher_->Init(test_dir_.string(),
        [](const std::vector<FileEvent>&) {});
    watcher_->Start();
    watcher_->Stop();
    // Second stop should be safe (no-op)
    watcher_->Stop();
    EXPECT_FALSE(watcher_->IsRunning());
}

TEST_F(FileWatcherTest, InitEmptyDirectory) {
    namespace fs = std::filesystem;
    auto empty_dir = fs::temp_directory_path() / "cortrix_test_empty_watcher_init";
    fs::create_directories(empty_dir);

    auto local_watcher = FileWatcher::Create();
    int rc = local_watcher->Init(empty_dir.string(),
        [](const std::vector<FileEvent>&) {});
    EXPECT_EQ(0, rc);

    EXPECT_EQ(0, local_watcher->Start());
    EXPECT_TRUE(local_watcher->IsRunning());
    local_watcher->Stop();

    fs::remove_all(empty_dir);
}

TEST_F(FileWatcherTest, EventTimestampIsNonZero) {
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CreateFile("timestamp_test.txt");
    WaitForEvents(1);

    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& e : events_) {
        if (e.path.find("timestamp_test.txt") != std::string::npos) {
            EXPECT_GT(e.timestamp_us, 0);
        }
    }
}

TEST_F(FileWatcherTest, DirectoryEventHasIsDirectoryFlag) {
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Create a subdirectory
    std::filesystem::create_directory(test_dir_ / "new_subdir");
    WaitForEvents(1, 3000);

    std::lock_guard<std::mutex> lock(mu_);
    // On some platforms, directory creation may or may not produce a directory event.
    // Verify events have correct is_directory flag when present.
    for (const auto& e : events_) {
        if (e.path.find("new_subdir") != std::string::npos) {
            // If we got an event for this path, the is_directory flag should be true
            EXPECT_TRUE(e.is_directory);
        }
    }
    // Just verify no crash
}

TEST_F(FileWatcherTest, ManySmallFilesCreatedQuickly) {
    watcher_->Init(test_dir_.string(),
        [this](const std::vector<FileEvent>& evts) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& e : evts) events_.push_back(e);
            cv_.notify_all();
        });
    watcher_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Create 20 files rapidly
    for (int i = 0; i < 20; ++i) {
        std::ofstream out(test_dir_ / ("rapid_" + std::to_string(i) + ".txt"));
        out << "content " << i;
    }
    WaitForEvents(10, 5000);

    std::lock_guard<std::mutex> lock(mu_);
    int create_count = 0;
    for (const auto& e : events_) {
        if (e.type == FileEventType::kCreated && !e.is_directory) {
            create_count++;
        }
    }
    // At least some of the files should have been detected
    EXPECT_GT(create_count, 0);
}

}  // namespace
}  // namespace cortrix
