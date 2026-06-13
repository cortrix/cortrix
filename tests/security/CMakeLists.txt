add_executable(cortrix_security_tests
    test_injection_attacks.cpp
    test_auth_bypass.cpp
    test_namespace_crossing.cpp
    test_malformed_input.cpp
)

target_link_libraries(cortrix_security_tests PRIVATE
    cortrix_core
    gtest_main
    gmock
)

target_include_directories(cortrix_security_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../mocks
    ${CMAKE_SOURCE_DIR}/include
)

include(GoogleTest)
gtest_discover_tests(cortrix_security_tests PROPERTIES LABELS security)
