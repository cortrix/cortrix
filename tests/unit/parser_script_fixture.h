#pragma once
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

namespace cortrix::spc {
namespace test {

/// gtest fixture base for parser-bridge tests: writes throwaway script / data
/// files under /tmp and cleans them up. Shared by the Docling + PaddleOCR + the
/// fallback-chain tests so the mock-bridge plumbing lives in one place.
class ScriptFixture : public ::testing::Test {
protected:
    std::string Write(const std::string& suffix, const std::string& content) {
        char tmpl[] = "/tmp/cortrix_f06_fix_XXXXXX";
        int fd = ::mkstemp(tmpl);
        if (fd >= 0) ::close(fd);
        std::string path = std::string(tmpl) + suffix;
        std::rename(tmpl, path.c_str());
        std::ofstream f(path);
        f << content;
        f.close();
        paths_.push_back(path);
        return path;
    }

    /// A python mock bridge that ignores its args and prints `json_literal`.
    std::string MockEmitting(const std::string& json_literal) {
        return Write(".py", "import sys\nprint('''" + json_literal + "''')\n");
    }

    void TearDown() override {
        for (const auto& p : paths_) std::remove(p.c_str());
    }

    std::vector<std::string> paths_;
};

}  // namespace test
}  // namespace cortrix::spc
