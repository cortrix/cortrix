#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace cortrix::test {

// Current test's name with path-hostile characters flattened, for embedding in
// per-test file/directory names (the F-1 race-family uniquifier idiom; QA
// 2026-07-12 Nit-3). TEST_P names carry '/' ("Case/0") and value-parameterized
// names can print arbitrary characters — a raw '/' would silently nest
// directories (or fail the open) the moment a fixture goes parameterized.
inline std::string SanitizedTestName() {
    std::string n = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::replace_if(
        n.begin(), n.end(),
        [](char c) { return std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_'; },
        '_');
    return n;
}

}  // namespace cortrix::test
