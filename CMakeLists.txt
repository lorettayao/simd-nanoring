cmake_minimum_required(VERSION 3.10)
project(simd_nanoring)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Platform-specific SIMD flags
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    # ARM: NEON is always available on AArch64
    set(SIMD_FLAGS "")
    message(STATUS "Target: ARM64 (NEON intrinsics)")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(SIMD_FLAGS "-mavx2 -mssse3")
    message(STATUS "Target: x86-64 (AVX2 intrinsics)")
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 ${SIMD_FLAGS} -march=native -Wall -Wextra -Wpedantic -Werror")

# Suppress specific warnings that fire in NEON intrinsic usage patterns
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-gnu-anonymous-struct -Wno-nested-anon-types")
endif()

# Threading
find_package(Threads REQUIRED)

# Main pipeline demo
add_executable(simd_nanoring src/main.cpp)
target_include_directories(simd_nanoring PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(simd_nanoring Threads::Threads)

# Experiment suite
add_executable(run_experiments src/experiments.cpp)
target_include_directories(run_experiments PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(run_experiments Threads::Threads)

# Mock data generator (kept for compatibility)
add_executable(generate_data tools/generator.cpp)
target_include_directories(generate_data PRIVATE ${CMAKE_SOURCE_DIR}/include)
