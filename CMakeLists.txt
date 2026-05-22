cmake_minimum_required(VERSION 3.10)
project(simd_nanoring)

# Enforce C++17 Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Industrial Compiler Flags 
# -O3: Maximum speed optimization
# -mavx2: Enables 256-bit SIMD registers
# -pthread: Required for your core-pinning and lock-free threads
# -Wall -Wextra -Wpedantic: Enables strict code auditing 
# -Werror: Treats all warnings as fatal compilation errors [cite: 107]
# -march=native: Optimizes assembly for your exact physical CPU 
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mavx2 -pthread -Wall -Wextra -Wpedantic -Werror -march=native")

# Add the main executable
add_executable(simd_nanoring src/main.cpp)

# Explicitly tell the compiler where to find your remaining headers
# (simd_parser.h and ring_buffer.h)
target_include_directories(simd_nanoring PRIVATE ${CMAKE_SOURCE_DIR}/include)