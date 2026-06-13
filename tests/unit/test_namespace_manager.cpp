#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include "cortrix/namespace/namespace_manager.h"

namespace cortrix {
namespace {

class NamespaceManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_ns_" + std::to_string(getpid()) + "_" +
                   std::to_string(counter_++);
        system(("mkdir -p " + tmp_dir_).c_str());
    }

    void TearDown() override {
        system(("rm -rf " + tmp_dir_).c_str());
    }

    std::string tmp_dir_;
    static int counter_;
};

int NamespaceManagerTest::counter_ = 0;

TEST_F(NamespaceManagerTest, InitCreatesDefault) {
    NamespaceManager mgr(tmp_dir_);
    Status s = mgr.Init();
    EXPECT_TRUE(s.ok());

    NamespaceInfo info;
    s = mgr.Get("default", &info);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(info.name, "default");
    EXPECT_GT(info.created_at, 0);
}

TEST_F(NamespaceManagerTest, InitLoadsExisting) {
    // Pre-write a namespaces.json
    std::string json_path = tmp_dir_ + "/namespaces.json";
    {
        std::ofstream f(json_path);
        f << R"({
            "version": 1,
            "namespaces": [
                {"name": "default", "created_at": 1000, "updated_at": 1000, "doc_count": 0, "block_count": 0},
                {"name": "test-ns", "created_at": 2000, "updated_at": 2000, "doc_count": 5, "block_count": 10}
            ]
        })";
    }

    NamespaceManager mgr(tmp_dir_);
    Status s = mgr.Init();
    EXPECT_TRUE(s.ok());

    NamespaceInfo info;
    s = mgr.Get("test-ns", &info);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(info.name, "test-ns");
    EXPECT_EQ(info.created_at, 2000);
    EXPECT_EQ(info.doc_count, 5);
    EXPECT_EQ(info.block_count, 10);
}

TEST_F(NamespaceManagerTest, CreateValid) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();

    Status s = mgr.Create("my-ns");
    EXPECT_TRUE(s.ok());

    NamespaceInfo info;
    s = mgr.Get("my-ns", &info);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(info.name, "my-ns");
    EXPECT_GT(info.created_at, 0);
}

TEST_F(NamespaceManagerTest, CreateInvalidNameEmpty) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    Status s = mgr.Create("");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(NamespaceManagerTest, CreateInvalidNameSpaces) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    Status s = mgr.Create("a b");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(NamespaceManagerTest, CreateInvalidNameTooLong) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    std::string long_name(65, 'x');
    Status s = mgr.Create(long_name);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(NamespaceManagerTest, CreateInvalidNameSpecialChars) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    Status s = mgr.Create("ns@#$");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(NamespaceManagerTest, CreateDuplicate) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    Status s = mgr.Create("dup-ns");
    EXPECT_TRUE(s.ok());
    s = mgr.Create("dup-ns");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kAlreadyExists);
}

TEST_F(NamespaceManagerTest, GetExisting) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    mgr.Create("get-test");

    NamespaceInfo info;
    Status s = mgr.Get("get-test", &info);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(info.name, "get-test");
    EXPECT_GT(info.created_at, 0);
    EXPECT_EQ(info.doc_count, 0);
}

TEST_F(NamespaceManagerTest, GetNotFound) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();

    NamespaceInfo info;
    Status s = mgr.Get("nonexistent", &info);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST_F(NamespaceManagerTest, ListAll) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    mgr.Create("ns-a");
    mgr.Create("ns-b");
    mgr.Create("ns-c");

    auto list = mgr.List();
    EXPECT_EQ(list.size(), 4u);  // default + 3 created
    // Should be sorted by name
    EXPECT_EQ(list[0].name, "default");
    EXPECT_EQ(list[1].name, "ns-a");
    EXPECT_EQ(list[2].name, "ns-b");
    EXPECT_EQ(list[3].name, "ns-c");
}

TEST_F(NamespaceManagerTest, ListFiltered) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    mgr.Create("ns1");
    mgr.Create("ns2");
    mgr.Create("ns3");

    auto list = mgr.List({"ns1", "ns2"});
    EXPECT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "ns1");
    EXPECT_EQ(list[1].name, "ns2");
}

TEST_F(NamespaceManagerTest, DeleteExisting) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    mgr.Create("to-delete");

    Status s = mgr.Delete("to-delete");
    EXPECT_TRUE(s.ok());

    NamespaceInfo info;
    s = mgr.Get("to-delete", &info);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST_F(NamespaceManagerTest, DeleteNotFound) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();

    Status s = mgr.Delete("nonexistent");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST_F(NamespaceManagerTest, DeleteDefault) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();

    Status s = mgr.Delete("default");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(s.message(), "Cannot delete the 'default' namespace");
}

TEST_F(NamespaceManagerTest, PersistenceRoundTrip) {
    // Create namespaces with first manager instance
    {
        NamespaceManager mgr(tmp_dir_);
        mgr.Init();
        mgr.Create("persist-ns");
    }

    // Load with new manager instance
    {
        NamespaceManager mgr(tmp_dir_);
        mgr.Init();

        NamespaceInfo info;
        Status s = mgr.Get("persist-ns", &info);
        EXPECT_TRUE(s.ok());
        EXPECT_EQ(info.name, "persist-ns");
    }
}

TEST_F(NamespaceManagerTest, ValidNameFormats) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();

    EXPECT_TRUE(mgr.Create("a").ok());
    EXPECT_TRUE(mgr.Create("abc123").ok());
    EXPECT_TRUE(mgr.Create("my-namespace").ok());
    EXPECT_TRUE(mgr.Create("my_namespace").ok());
    EXPECT_TRUE(mgr.Create("ABC").ok());
    EXPECT_TRUE(mgr.Create(std::string(64, 'x')).ok());  // max length
}

// ============================================================
// Coverage Boost: Backup recovery and edge cases
// ============================================================

TEST_F(NamespaceManagerTest, RecoverFromBackup) {
    // Create a valid backup file
    std::string bak_path = tmp_dir_ + "/namespaces.json.bak";
    {
        std::ofstream f(bak_path);
        f << R"({
            "version": 1,
            "namespaces": [
                {"name": "default", "created_at": 500, "updated_at": 500, "doc_count": 0, "block_count": 0},
                {"name": "recovered-ns", "created_at": 600, "updated_at": 600, "doc_count": 3, "block_count": 9}
            ]
        })";
    }

    // Don't create the primary file - force backup recovery
    NamespaceManager mgr(tmp_dir_);
    Status s = mgr.Init();
    EXPECT_TRUE(s.ok());

    NamespaceInfo info;
    s = mgr.Get("recovered-ns", &info);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(info.name, "recovered-ns");
    EXPECT_EQ(info.doc_count, 3);
}

TEST_F(NamespaceManagerTest, CorruptPrimaryFallsToBackup) {
    // Write corrupt primary file
    std::string json_path = tmp_dir_ + "/namespaces.json";
    {
        std::ofstream f(json_path);
        f << "{{{{not valid json}}}}";
    }

    // Write valid backup
    std::string bak_path = tmp_dir_ + "/namespaces.json.bak";
    {
        std::ofstream f(bak_path);
        f << R"({
            "version": 1,
            "namespaces": [
                {"name": "default", "created_at": 100, "updated_at": 100, "doc_count": 0, "block_count": 0},
                {"name": "from-backup", "created_at": 200, "updated_at": 200, "doc_count": 1, "block_count": 2}
            ]
        })";
    }

    NamespaceManager mgr(tmp_dir_);
    Status s = mgr.Init();
    EXPECT_TRUE(s.ok());

    NamespaceInfo info;
    s = mgr.Get("from-backup", &info);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(info.name, "from-backup");
}

TEST_F(NamespaceManagerTest, NeitherPrimaryNorBackupStartsFresh) {
    // Empty directory - no json files at all
    NamespaceManager mgr(tmp_dir_);
    Status s = mgr.Init();
    EXPECT_TRUE(s.ok());

    // Should still have "default"
    NamespaceInfo info;
    s = mgr.Get("default", &info);
    EXPECT_TRUE(s.ok());

    // Should only have "default"
    auto list = mgr.List();
    EXPECT_EQ(list.size(), 1u);
}

TEST_F(NamespaceManagerTest, InitWithExistingDefault) {
    // Pre-write a file with "default" already there
    std::string json_path = tmp_dir_ + "/namespaces.json";
    {
        std::ofstream f(json_path);
        f << R"({
            "version": 1,
            "namespaces": [
                {"name": "default", "created_at": 999, "updated_at": 999, "doc_count": 10, "block_count": 50}
            ]
        })";
    }

    NamespaceManager mgr(tmp_dir_);
    Status s = mgr.Init();
    EXPECT_TRUE(s.ok());

    // Should not create a duplicate default
    auto list = mgr.List();
    int default_count = 0;
    for (auto& ns : list) {
        if (ns.name == "default") default_count++;
    }
    EXPECT_EQ(default_count, 1);

    // Should preserve existing default's data
    NamespaceInfo info;
    mgr.Get("default", &info);
    EXPECT_EQ(info.doc_count, 10);
}

TEST_F(NamespaceManagerTest, ListFilteredNonExistent) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    mgr.Create("exists");

    // Filter with a namespace that doesn't exist
    auto list = mgr.List({"nonexistent"});
    EXPECT_EQ(list.size(), 0u);
}

TEST_F(NamespaceManagerTest, ListFilteredPartial) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();
    mgr.Create("ns1");
    mgr.Create("ns2");
    mgr.Create("ns3");

    // Filter with mix of existing and non-existing
    auto list = mgr.List({"ns1", "nonexistent", "ns3"});
    EXPECT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "ns1");
    EXPECT_EQ(list[1].name, "ns3");
}

TEST_F(NamespaceManagerTest, DeleteThenRecreate) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();

    mgr.Create("temp");
    Status s = mgr.Delete("temp");
    EXPECT_TRUE(s.ok());

    // Re-create should work
    s = mgr.Create("temp");
    EXPECT_TRUE(s.ok());

    NamespaceInfo info;
    s = mgr.Get("temp", &info);
    EXPECT_TRUE(s.ok());
}

TEST_F(NamespaceManagerTest, CreateDefaultNameFails) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();

    // "default" already exists, creating again should fail with AlreadyExists
    Status s = mgr.Create("default");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kAlreadyExists);
}

TEST_F(NamespaceManagerTest, SaveCreatesBackup) {
    NamespaceManager mgr(tmp_dir_);
    mgr.Init();

    // First save creates the primary file
    mgr.Create("backup-test");

    // Primary file should exist
    EXPECT_TRUE(std::ifstream(tmp_dir_ + "/namespaces.json").good());

    // Now create another namespace (triggers another save, which creates backup)
    mgr.Create("backup-test2");

    // Backup file should now exist (the previous namespaces.json was renamed to .bak)
    EXPECT_TRUE(std::ifstream(tmp_dir_ + "/namespaces.json.bak").good());
}

}  // namespace
}  // namespace cortrix
