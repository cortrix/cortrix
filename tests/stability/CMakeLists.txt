add_executable(cortrix_stability_tests
    test_concurrent_stress.cpp
    test_sustained_load.cpp
)

target_link_libraries(cortrix_stability_tests PRIVATE
    cortrix_core
    gtest_main
    gmock
)

target_include_directories(cortrix_stability_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../mocks
    ${CMAKE_SOURCE_DIR}/include
)

include(GoogleTest)
gtest_discover_tests(cortrix_stability_tests PROPERTIES LABELS stability)
