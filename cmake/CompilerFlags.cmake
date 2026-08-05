# CompilerFlags.cmake — C++17 + warnings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
endif()

# Opt-in sanitizers for concurrency / memory verification (index S5 DoD: TSAN; also
# usable by any feature's stress tests). Off by default so normal builds are
# unaffected. Usage:
#   cmake -B build-tsan -DCORTRIX_SANITIZE=thread   (data races)
#   cmake -B build-asan -DCORTRIX_SANITIZE=address  (memory errors)
set(CORTRIX_SANITIZE "" CACHE STRING "Enable a sanitizer: thread | address | undefined")
if(CORTRIX_SANITIZE)
    message(STATUS "Sanitizer enabled: ${CORTRIX_SANITIZE}")
    add_compile_options(-fsanitize=${CORTRIX_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${CORTRIX_SANITIZE})
endif()
