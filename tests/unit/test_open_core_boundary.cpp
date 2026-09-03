// Open-Core boundary guard (ARCHITECTURE Sec.1.0.10 / GEN-OpenCore-Boundary).
//
// The CE tree must contain ZERO enterprise coupling: no enterprise build
// macros, no #ifdef on an enterprise flag, no `namespace enterprise`, no IEnterprise*
// interface, no #include of an enterprise header, and no CREATE TABLE for an Ent
// extension table (audit_log_extension / agent_trace_extension /
// interaction_sources_extension). Ent depends on CE one-way (add_subdirectory + link
// cortrix_core); CE never knows Ent exists.
//
// This test scans the real CE src/ + include/ trees at run time and fails the build
// if any forbidden pattern reappears -- an automated guard against boundary
// regression (previously only asserted by hand). CORTRIX_CE_SOURCE_DIR is injected by
// CMake (target_compile_definitions) and points at the CE repo root.
//
// "enterprise" as an ADJECTIVE in prose comments is allowed (e.g. "the enterprise
// build overrides this"); what is forbidden is enterprise as a code identifier
// (macro / namespace / type / include / table). The patterns below target the code
// forms only, so descriptive comments do not trip the guard.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace cortrix {
namespace {

namespace fs = std::filesystem;

#ifndef CORTRIX_CE_SOURCE_DIR
#error "CORTRIX_CE_SOURCE_DIR must be defined by CMake (the CE repo root)"
#endif

// One forbidden pattern + a human label for the failure message.
struct Rule {
    std::regex re;
    const char* label;
};

// Collect every C/C++ source + header under the CE src/ and include/ trees.
std::vector<fs::path> CollectCeSources() {
    std::vector<fs::path> out;
    const fs::path root(CORTRIX_CE_SOURCE_DIR);
    for (const char* sub : {"src", "include"}) {
        const fs::path dir = root / sub;
        if (!fs::exists(dir)) continue;
        for (auto it = fs::recursive_directory_iterator(dir);
             it != fs::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            const std::string ext = it->path().extension().string();
            if (ext == ".cpp" || ext == ".cc" || ext == ".h" || ext == ".hpp") {
                out.push_back(it->path());
            }
        }
    }
    return out;
}

std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

class OpenCoreBoundaryTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { sources_ = CollectCeSources(); }
    static std::vector<fs::path> sources_;

    // Fail if any CE source matches `rule`. Reports the first offending file:line.
    void ExpectNoMatch(const Rule& rule) {
        for (const auto& path : sources_) {
            std::ifstream in(path);
            std::string line;
            int n = 0;
            while (std::getline(in, line)) {
                ++n;
                if (std::regex_search(line, rule.re)) {
                    ADD_FAILURE() << "Open-Core boundary violation [" << rule.label
                                  << "] at " << path.string() << ":" << n << "\n  "
                                  << line;
                }
            }
        }
    }
};

std::vector<fs::path> OpenCoreBoundaryTest::sources_;

// Reverse sentinel: the scanner must actually read a non-trivial CE tree. Without
// this, a broken path / empty glob would make every "no match" assertion vacuously
// pass. We require a healthy file count and that a known CE token IS found.
TEST_F(OpenCoreBoundaryTest, ScannerReadsNonEmptyCeTree) {
    ASSERT_GT(sources_.size(), 100u)
        << "expected to scan >100 CE source files; got " << sources_.size()
        << " (CORTRIX_CE_SOURCE_DIR wrong?)";
    bool saw_cortrix_token = false;
    for (const auto& path : sources_) {
        if (ReadFile(path).find("namespace cortrix") != std::string::npos) {
            saw_cortrix_token = true;
            break;
        }
    }
    EXPECT_TRUE(saw_cortrix_token)
        << "no `namespace cortrix` found in any scanned file -- scanner is not "
           "reading real CE sources";
}

// 1. No enterprise build macro (e.g. #define/#ifdef CORTRIX_ENTERPRISE, WITH_ENTERPRISE).
TEST_F(OpenCoreBoundaryTest, NoEnterpriseBuildMacro) {
    ExpectNoMatch({std::regex(R"(\b(CORTRIX_ENTERPRISE|WITH_ENTERPRISE|CORTRIX_ENT|ENABLE_ENTERPRISE)\b)"),
                   "enterprise build macro"});
}

// 2. No #ifdef / #if on any enterprise flag (conditional compilation for Ent).
TEST_F(OpenCoreBoundaryTest, NoEnterpriseIfdef) {
    ExpectNoMatch({std::regex(R"(#\s*if(def|ndef)?\b.*\b(ENTERPRISE|ENT_|WITH_ENT))",
                              std::regex::icase),
                   "enterprise #ifdef"});
}

// 3. No `namespace enterprise` (Ent code must not live in the CE tree).
TEST_F(OpenCoreBoundaryTest, NoEnterpriseNamespace) {
    ExpectNoMatch({std::regex(R"(namespace\s+enterprise\b)"), "namespace enterprise"});
}

// 4. No IEnterprise* / Enterprise* interface or class declared in CE.
TEST_F(OpenCoreBoundaryTest, NoEnterpriseTypeOrInterface) {
    ExpectNoMatch({std::regex(R"(\b(class|struct)\s+(I?Enterprise[A-Za-z0-9_]*)\b)"),
                   "enterprise class/interface"});
}

// 5. No #include of an enterprise header (CE must not consume Ent code).
TEST_F(OpenCoreBoundaryTest, NoEnterpriseInclude) {
    ExpectNoMatch({std::regex(R"(#\s*include\s*[<"][^>"]*enterprise[^>"]*[>"])",
                              std::regex::icase),
                   "enterprise #include"});
}

// 6. No CREATE TABLE for an Ent extension table -- those are Ent-side downstream
//    migrations keyed by the CE table id, never defined in CE DDL.
TEST_F(OpenCoreBoundaryTest, NoEntExtensionTableDdl) {
    ExpectNoMatch({std::regex(
                       R"(CREATE\s+TABLE[^;]*\b(audit_log_extension|agent_trace_extension|interaction_sources_extension)\b)",
                       std::regex::icase),
                   "Ent extension table CREATE TABLE"});
}

}  // namespace
}  // namespace cortrix
