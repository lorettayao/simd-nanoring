cmake_minimum_required(VERSION 3.10)
project(simd_nanoring)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Industrial Compiler Flags (-Werror, AVX2, Native Architecture)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mavx2 -pthread -Wall -Wextra -Wpedantic -Werror -march=native")

# Find the zlib library on your lab server
find_package(ZLIB REQUIRED)

add_executable(simd_nanoring src/main.cpp)

# Link zlib to your executable
target_link_libraries(simd_nanoring PRIVATE ZLIB::ZLIB)

# Compile the generator
g++ -O3 tools/generator.cpp -o generate_mock_data

# Run it
./generate_mock_data