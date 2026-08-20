cmake_minimum_required(VERSION 3.16)
project(custom_memory_allocator LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MEMPOOL_BUILD_TESTS "Build the unit test suite" ON)
option(MEMPOOL_BUILD_BENCHMARKS "Build the benchmark suite" ON)
option(MEMPOOL_ENABLE_SANITIZERS "Build with -fsanitize=address,undefined" OFF)

# ---------------------------------------------------------------------------
# Core library (header-only)
# ---------------------------------------------------------------------------
add_library(memory_pool INTERFACE)
target_include_directories(memory_pool INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(memory_pool INTERFACE cxx_std_17)

# Common warning / sanitizer flags applied to first-party targets only
# (kept off FetchContent'd third-party targets like GoogleTest).
add_library(project_warnings INTERFACE)
if(MSVC)
    target_compile_options(project_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(project_warnings INTERFACE -Wall -Wextra -Wpedantic -g)
endif()

if(MEMPOOL_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(project_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(project_warnings INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Demo / CLI driver
# ---------------------------------------------------------------------------
add_executable(memory_pool_demo src/main.cpp)
target_link_libraries(memory_pool_demo PRIVATE memory_pool project_warnings)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MEMPOOL_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    # Keep GoogleTest's own runtime/warning config isolated from ours.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(unit_tests
        tests/test_pool.cpp
        tests/test_refptr.cpp
    )
    target_link_libraries(unit_tests PRIVATE memory_pool project_warnings GTest::gtest GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(unit_tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(MEMPOOL_BUILD_BENCHMARKS)
    add_executable(bench_allocator benchmarks/bench_allocator.cpp)
    target_link_libraries(bench_allocator PRIVATE memory_pool project_warnings)

    # Benchmarks should reflect real optimized performance regardless of
    # the top-level build type.
    if(NOT MSVC)
        target_compile_options(bench_allocator PRIVATE -O3)
    else()
        target_compile_options(bench_allocator PRIVATE /O2)
    endif()
endif()
