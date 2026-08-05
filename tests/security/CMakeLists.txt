add_executable(cortrix_security_tests
    test_injection_attacks.cpp
    test_auth_bypass.cpp
    test_namespace_crossing.cpp
    test_malformed_input.cpp
    # --- coverage Wave 2: systematic security matrices ---
    test_sec_anti_enumeration.cpp
    test_sec_rbac_matrix.cpp
    test_sec_jwt_hardening.cpp
    test_sec_injection_ext.cpp
    test_sec_tenant_isolation.cpp
    # --- SEC-BATCH: batch inline-content materialization ---
    test_sec_batch_materialize.cpp
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
